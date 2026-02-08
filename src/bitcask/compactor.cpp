#include "bitcask/compactor.hpp"

namespace bitcask
{
    kio::Task<kio::Result<CompactionResult>> Compactor::CompactFiles(kio::IoContext& ctx,
                                                                     const std::vector<uint64_t>& src_file_ids,
                                                                     DataFile& dst_file)
    {
        CompactionResult result;

        kio::IoBuffer in_buf(kDefaultInBufReadSize);
        kio::IoBuffer out_buf(kDefaultOutBufWriteSize);

        auto& keydir = io_.GetKeyDir();

        // Track where we are writing in the destination file
        uint64_t dst_write_offset = dst_file.Size();

        // We track the logical offset in the destination file for the *next* entry to be written.
        // This includes data currently sitting in out_buf.
        uint64_t dst_logical_offset = dst_write_offset;

        for (const uint64_t src_id : src_file_ids)
        {
            const auto src_path = config_.directory / std::format("partition_{}/data_{}.db", io_.PartitionID(), src_id);
            int src_fd = KIO_CO_TRY(co_await io_.GetFDCache().GetOrOpen(ctx, src_id, src_path));

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
                        // If a file is done, and we have partial bytes, it's corruption/truncation.
                        if (file_done)
                        {
                            ALOG_WARN("Compaction: Truncated entry at end of file {}", src_id);
                            // Force exit inner loop to finish this file
                            in_buf.Clear();
                        }
                        // Break the inner loop to read more data
                        break;
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
                    if (auto it = keydir.find(key_view); it != keydir.end())
                    {
                        // It is live ONLY if the KeyDir points to *this specific file* and *this specific offset*
                        if (it->second.file_id == src_id && it->second.offset == src_entry_pos)
                        {
                            is_live = true;
                        }
                    }

                    if (is_live)
                    {
                        // -----------------------------------------------
                        // 4. Append to Output
                        // -----------------------------------------------
                        out_buf.Append(span.subspan(0, total_size));
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
        if (out_buf.ReadableBytes() > 0)
        {
            auto out_span = out_buf.ReadableSpan();
            KIO_CO_TRY(co_await kio::AsyncWriteExact(ctx, dst_file.Fd(), out_span, dst_write_offset));
            out_buf.Clear();
        }

        ALOG_DEBUG("Compaction: Syncing dst file (fd={})", dst_file.Fd());
        KIO_CO_TRY(co_await kio::AsyncFdatasync(ctx, dst_file.Fd()));

        co_return result;
    }

    kio::Task<kio::Result<void>> Compactor::Compact(kio::IoContext& ctx)
    {
        auto partition_id = io_.PartitionID();
        auto& keydir = io_.GetKeyDir();

        if (compaction_running_.load(std::memory_order_acquire))
        {
            co_return {};
        }

        const auto fragmented_files = FindFragmentedFiles();

        if (fragmented_files.empty())
        {
            ALOG_DEBUG("Partition {}: no files to compact", partition_id);
            co_return {};
        }

        ALOG_INFO("Partition {}: compacting {} files", partition_id, fragmented_files.size());

        compaction_running_.store(true, std::memory_order_release);

        // Generate destination file ID
        const uint64_t dst_file_id = io_.FdGen().Next();
        const auto dst_path = config_.directory / std::format("partition_{}/data_{}.db", partition_id, dst_file_id);
        const int dst_fd = KIO_CO_TRY(co_await kio::AsyncOpen(ctx, dst_path, config_.write_flags, config_.file_mode)).
            Get();

        DataFile dst_file(dst_fd, dst_file_id, config_);

        const auto result = co_await CompactFiles(ctx, fragmented_files, dst_file);

        if (!result.has_value())
        {
            ALOG_ERROR("Partition {}: compaction failed: {}", partition_id, result.error().message());
            stats_.compactions_failed++;
            compaction_running_.store(false, std::memory_order_release);

            // Clean up the partial destination file
            const auto path = config_.directory / std::format("partition_{}/data_{}.db", partition_id, dst_file_id);
            KIO_CO_TRY(co_await AsyncUnlink(ctx, AT_FDCWD, path, 0));
            // TODO fix this error category
            co_return std::unexpected(std::error_code());
        }

        for (const auto& hint : result->new_hints)
        {
            if (auto it = keydir.find(hint.key); it != keydir.end())
            {
                it->second.file_id = dst_file_id;
                it->second.offset = hint.offset;
                it->second.total_size = hint.size;
                it->second.timestamp_ns = hint.timestamp_ns;
            }
        }

        ALOG_INFO("Partition {}: compaction succeeded, cleaning up {} source files", partition_id,
                  fragmented_files.size());
        stats_.compactions_total++;

        // Delete source files after successful compaction
        uint64_t bytes_reclaimed = 0;
        for (const uint64_t src_id : fragmented_files)
        {
            // Remove from FD cache first
            co_await io_.GetFDCache().Remove(ctx, src_id);

            // Track reclaimed bytes
            if (auto it = stats_.data_files.find(src_id); it != stats_.data_files.end())
            {
                bytes_reclaimed += it->second.total_bytes;
                stats_.data_files.erase(it);
            }

            // Delete data file
            const auto data_path = config_.directory / std::format("partition_{}/data_{}.db", partition_id, src_id);
            KIO_CO_TRY(co_await kio::AsyncUnlink(ctx, AT_FDCWD, data_path, 0));

            // Delete a hint file if exists
            const auto hint_path = config_.directory / std::format("partition_{}/hint_{}.ht", partition_id, src_id);
            if (std::filesystem::exists(hint_path))
            {
                KIO_CO_TRY(co_await kio::AsyncUnlink(ctx, AT_FDCWD, hint_path, 0));
            }

            stats_.files_compacted_total++;
        }

        stats_.bytes_reclaimed_total += bytes_reclaimed;
        stats_.compaction_running = false;

        ALOG_INFO("Partition {}: reclaimed {} bytes from {} files", partition_id, bytes_reclaimed,
                  fragmented_files.size());

        co_return {};
    }

    std::vector<uint64_t> Compactor::FindFragmentedFiles() const
    {
        // Iterate only over candidates we specifically marked
        std::vector<uint64_t> res;
        for (const auto& fid : compaction_candidates_)
        {
            // Sanity check: ensure a file exists in stats and isn't active
            if (io_.ActiveFileID() || io_.ActiveFileID() == fid)
            {
                continue;
            }
            if (stats_.data_files.contains(fid))
            {
                res.push_back(fid);
            }
        }
        return res;
    }

    bool Compactor::ShouldCompactFile(const uint64_t file_id) const
    {
        const auto it = stats_.data_files.find(file_id);
        if (it == stats_.data_files.end())
        {
            return false;
        }
        return it->second.Fragmentation() >= config_.fragmentation_threshold;
    }

    void Compactor::SignalCompaction(const uint64_t file_id)
    {
        // Do not signal already tracked files
        if (compaction_candidates_.contains(file_id))
        {
            return;
        }
        compaction_candidates_.insert(file_id);
        compaction_signal_.Signal();
    }
} // namespace bitcask

