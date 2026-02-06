#include "bitcask/compactor.hpp"

namespace bitcask
{
    kio::Task<kio::Result<CompactionResult>> CompactFiles(
        BitcaskConfig& cfg,
        kio::IoContext& ctx,
        const std::vector<uint64_t>& src_file_ids,
        DataFile& dst_file,
        FDCache& fd_cache,
        const KeyDir& key_dir,
        const uint64_t partition_id
    )
    {
        CompactionResult result;

        // Buffers for zero-copy(ish) IO
        kio::IoBuffer in_buf(kDefaultInBufReadSize);
        kio::IoBuffer out_buf(kDefaultOutBufWriteSize);

        // Track where we are writing in the destination file
        uint64_t dst_write_offset = dst_file.Size();

        // We track the logical offset in the destination file for the *next* entry to be written.
        // This includes data currently sitting in out_buf.
        uint64_t dst_logical_offset = dst_write_offset;

        for (const uint64_t src_id : src_file_ids)
        {
            const auto src_path = cfg.directory / std::format("partition_{}/data_{}.db", partition_id, src_id);
            int src_fd = KIO_CO_TRY(co_await fd_cache.GetOrOpen(ctx, src_id, src_path));

            uint64_t src_read_ptr = 0; // Physical offset for syscall reading
            uint64_t src_entry_pos = 0; // Logical offset of the entry being processed

            in_buf.Clear();

            bool file_done = false;

            while (!file_done)
            {
                // -------------------------------------------------------
                // 1. Fill Input Buffer
                // -------------------------------------------------------

                // Ensure we have space to read more data
                in_buf.EnsureWritableBytes(kDefaultInBufReadSize);

                auto wr_span = in_buf.WritableBytesSpan();
                auto bytes_read = KIO_CO_TRY(co_await kio::AsyncRead(ctx, src_fd, wr_span, src_read_ptr));

                if (bytes_read > 0)
                {
                    in_buf.Commit(bytes_read);
                    src_read_ptr += bytes_read;
                }
                else
                {
                    // EOF reached
                    file_done = true;
                }

                // -------------------------------------------------------
                // 2. Parse and Process Entries
                // -------------------------------------------------------
                while (in_buf.ReadableBytes() >= kEntryFixedHeaderSize)
                {
                    // Peek the header
                    auto span = in_buf.ReadableSpan();
                    const auto* ptr = span.data();

                    // Decode Header: [CRC(4)][TS(8)][Flag(1)][KeyLen(4)][ValLen(4)]
                    // We need KeyLen (offset 13) and ValLen (offset 17) to know full size
                    const auto key_len = ReadLe<uint32_t>(ptr + 13);
                    const auto val_len = ReadLe<uint32_t>(ptr + 17);
                    const uint32_t total_size = kEntryFixedHeaderSize + key_len + val_len;

                    // Do we have the full entry in the buffer?
                    if (in_buf.ReadableBytes() < total_size)
                    {
                        // Not enough data yet.
                        // If file is done and we have partial bytes, it's a corruption/truncation.
                        if (file_done)
                        {
                            ALOG_WARN("Compaction: Truncated entry at end of file {}", src_id);
                            // Force exit inner loop to finish this file
                            in_buf.Clear();
                        }
                        break; // Break inner loop to read more data
                    }

                    // ---------------------------------------------------
                    // 3. Liveness Check
                    // ---------------------------------------------------

                    // Extract Key View (without copying)
                    std::string_view key_view(
                        reinterpret_cast<const char*>(ptr + kEntryFixedHeaderSize),
                        key_len
                    );

                    bool is_live = false;

                    // Look up in the in-memory index (KeyDir)
                    if (auto it = key_dir.find(key_view); it != key_dir.end())
                    {
                        ALOG_DEBUG("Compaction: Found key '{}' in KeyDir: file_id={}, offset={}",
                                   key_view, it->second.file_id, it->second.offset);
                        ALOG_DEBUG("  Current position: src_id={}, src_entry_pos={}", src_id, src_entry_pos);

                        // It is live ONLY if the KeyDir points to *this specific file* and *this specific offset*
                        if (it->second.file_id == src_id && it->second.offset == src_entry_pos)
                        {
                            is_live = true;
                            ALOG_DEBUG("  -> Entry is LIVE");
                        }
                        else
                        {
                            ALOG_DEBUG("  -> Entry is STALE (file_id or offset mismatch)");
                        }
                    }
                    else
                    {
                        ALOG_DEBUG("Compaction: Key '{}' NOT in KeyDir (deleted)", key_view);
                    }

                    if (is_live)
                    {
                        // -----------------------------------------------
                        // 4. Append to Output
                        // -----------------------------------------------
                        ALOG_DEBUG("Compaction: Appending live entry '{}' (size={}) to output buffer",
                                   key_view, total_size);

                        out_buf.Append(span.subspan(0, total_size));
                        // FIX: Must commit appended bytes so they become readable for the flush check
                        out_buf.Commit();

                        // Extract timestamp for the Hint
                        auto ts = ReadLe<uint64_t>(ptr + 4);

                        // Record Hint for the NEW location
                        result.new_hints.emplace_back(
                            ts,
                            dst_logical_offset, // New Offset in dst file
                            total_size,
                            std::string(key_view)
                        );

                        dst_logical_offset += total_size;
                    }
                    else
                    {
                        result.bytes_reclaimed += total_size;
                    }

                    // Advance pointers
                    in_buf.Consume(total_size);
                    src_entry_pos += total_size;

                    // ---------------------------------------------------
                    // 5. Periodic Output Flush
                    // ---------------------------------------------------
                    if (out_buf.ReadableBytes() >= kDefaultOutBufWriteSize)
                    {
                        auto out_span = out_buf.ReadableSpan();
                        KIO_CO_TRY(co_await kio::AsyncWriteExact(ctx, dst_file.Fd(), out_span, dst_write_offset));

                        dst_write_offset += out_span.size();
                        out_buf.Clear();
                    }
                }
            }
        }

        // -------------------------------------------------------
        // 6. Final Flush and Sync
        // -------------------------------------------------------
        ALOG_DEBUG("Compaction: Final flush - out_buf has {} readable bytes", out_buf.ReadableBytes());

        if (out_buf.ReadableBytes() > 0)
        {
            auto out_span = out_buf.ReadableSpan();
            ALOG_DEBUG("Compaction: Writing {} bytes to dst file at offset {}", out_span.size(), dst_write_offset);

            KIO_CO_TRY(co_await kio::AsyncWriteExact(ctx, dst_file.Fd(), out_span, dst_write_offset));
            dst_write_offset += out_span.size();
            out_buf.Clear();
        }

        // CRITICAL: Sync data to disk before returning
        // The test's ReadAllEntries needs the data to be physically written
        ALOG_DEBUG("Compaction: Syncing dst file (fd={})", dst_file.Fd());
        KIO_CO_TRY(co_await kio::AsyncFdatasync(ctx, dst_file.Fd()));
        ALOG_INFO("Compaction complete: {} hints, {} bytes reclaimed",
                  result.new_hints.size(), result.bytes_reclaimed);

        co_return result;
    }
} // namespace bitcask
