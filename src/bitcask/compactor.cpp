#include "bitcask/compactor.hpp"

namespace bitcask
{
    // Helper: Logic to process a chunk of memory (Pure logic + Output Buffer Management)
    kio::Task<kio::Result<void>> Compactor::ProcessBufferEntries(kio::IoContext& ctx, kio::IoBuffer& in_buf,
                                                                 kio::IoBuffer& out_buf, DataFile& dst_file,
                                                                 CompactionContext& c_ctx, const bool is_eof)
    {
        auto& keydir = io_.GetKeyDir();

        while (in_buf.ReadableBytes() >= kEntryFixedHeaderSize)
        {
            auto span = in_buf.ReadableSpan();
            const auto* ptr = span.data();

            // 1. Parse Header
            // [0-3] CRC, [4-11] Timestamp, [12] Flag, [13-16] KeyLen, [17-20] ValLen
            const auto key_len = ReadLe<uint32_t>(ptr + 13);
            const auto val_len = ReadLe<uint32_t>(ptr + 17);
            const uint32_t total_size = kEntryFixedHeaderSize + key_len + val_len;

            // 2. Bound Check
            if (in_buf.ReadableBytes() < total_size)
            {
                if (is_eof)
                {
                    ALOG_WARN("Compaction: Truncated entry at end of file {}", c_ctx.current_src_id);
                    in_buf.Clear();
                }
                break; // Need more data
            }

            // 3. Liveness Check
            std::string_view key_view(reinterpret_cast<const char*>(ptr + kEntryFixedHeaderSize), key_len);

            bool is_live = false;
            if (auto it = keydir.find(key_view); it != keydir.end())
            {
                // Strict check: Must match specific file and offset
                if (it->second.file_id == c_ctx.current_src_id && it->second.offset == c_ctx.current_src_offset)
                {
                    is_live = true;
                }
            }

            // 4. Action
            if (is_live)
            {
                out_buf.Append(span.subspan(0, total_size));
                out_buf.Commit();

                auto ts = ReadLe<uint64_t>(ptr + 4);

                c_ctx.result.new_hints.emplace_back(ts, c_ctx.dst_logical_offset, total_size, std::string(key_view));

                c_ctx.dst_logical_offset += total_size;
            }
            else
            {
                c_ctx.result.bytes_reclaimed += total_size;
            }

            // 5. Advance Input
            in_buf.Consume(total_size);
            c_ctx.current_src_offset += total_size;

            // 6. Flush Output if full
            if (out_buf.ReadableBytes() >= kDefaultOutBufWriteSize)
            {
                auto out_span = out_buf.ReadableSpan();
                KIO_CO_TRY_LOG(co_await kio::AsyncWriteExact(ctx, dst_file.RawFd(), out_span, c_ctx.dst_write_offset));

                c_ctx.dst_write_offset += out_span.size();
                out_buf.Clear();
            }
        }
        co_return {};
    }

    kio::Task<kio::Result<CompactionResult>> Compactor::CompactFiles(kio::IoContext& ctx,
                                                                     const std::vector<uint64_t>& src_file_ids,
                                                                     DataFile& dst_file)
    {
        // Use a context struct to avoid passing arguments repeatedly
        CompactionContext c_ctx;
        c_ctx.dst_write_offset = dst_file.Size();
        c_ctx.dst_logical_offset = c_ctx.dst_write_offset;

        kio::IoBuffer in_buf(kDefaultInBufReadSize);
        kio::IoBuffer out_buf(kDefaultOutBufWriteSize);

        for (const uint64_t src_id : src_file_ids)
        {
            c_ctx.current_src_id = src_id;
            c_ctx.current_src_offset = 0; // Logical offset in source file

            const auto src_path = config_.directory / std::format("partition_{}/data_{}.db", io_.PartitionID(), src_id);
            auto src_fd = KIO_CO_TRY_LOG(co_await io_.GetFDCache().GetOrOpen(ctx, src_id, src_path));

            uint64_t src_read_ptr = 0; // Physical syscall offset
            in_buf.Clear();
            bool file_done = false;

            while (!file_done)
            {
                in_buf.EnsureWritableBytes(kDefaultInBufReadSize);
                auto wr_span = in_buf.WritableSpan();

                auto bytes_read_res = co_await kio::AsyncRead(ctx, src_fd->Get(), wr_span, src_read_ptr);
                if (!bytes_read_res)
                    co_return std::unexpected(bytes_read_res.error());

                size_t bytes_read = *bytes_read_res;

                if (bytes_read > 0)
                {
                    in_buf.Commit(bytes_read);
                    src_read_ptr += bytes_read;
                }
                else
                {
                    file_done = true;
                }

                // Process buffer content
                KIO_CO_TRY(co_await ProcessBufferEntries(ctx, in_buf, out_buf, dst_file, c_ctx, file_done));
            }
        }

        // Final Flush
        if (out_buf.ReadableBytes() > 0)
        {
            auto out_span = out_buf.ReadableSpan();
            KIO_CO_TRY(co_await kio::AsyncWriteExact(ctx, dst_file.RawFd(), out_span, c_ctx.dst_write_offset));
            c_ctx.dst_write_offset += out_span.size();
            out_buf.Clear();
        }

        ALOG_DEBUG("Compaction: Syncing dst file (fd={})", dst_file.RawFd());
        KIO_CO_TRY(co_await kio::AsyncFdatasync(ctx, dst_file.RawFd()));

        co_return c_ctx.result;
    }

    void Compactor::UpdateKeyDir(const std::vector<HintEntry>& new_hints, uint64_t dst_file_id) const
    {
        auto& keydir = io_.GetKeyDir();
        for (const auto& hint : new_hints)
        {
            if (auto it = keydir.find(hint.key); it != keydir.end())
            {
                // Note: This logic assumes no overwrites happened to the *old* file during compaction.
                it->second.file_id = dst_file_id;
                it->second.offset = hint.offset;
                it->second.total_size = hint.size;
                it->second.timestamp_ns = hint.timestamp_ns;
            }
        }
    }

    kio::Task<kio::Result<uint64_t>> Compactor::DeleteSourceFiles(kio::IoContext& ctx,
                                                                  const std::vector<uint64_t>& fragmented_files)
    {
        uint64_t bytes_reclaimed = 0;
        auto partition_id = io_.PartitionID();

        for (const uint64_t src_id : fragmented_files)
        {
            io_.GetFDCache().Remove(src_id);

            if (auto it = stats_.data_files.find(src_id); it != stats_.data_files.end())
            {
                bytes_reclaimed += it->second.total_bytes;
                stats_.data_files.erase(it);
            }

            const auto data_path = config_.directory / std::format("partition_{}/data_{}.db", partition_id, src_id);
            KIO_CO_TRY_LOG(co_await kio::AsyncUnlink(ctx, AT_FDCWD, data_path, 0));

            const auto hint_path = config_.directory / std::format("partition_{}/hint_{}.ht", partition_id, src_id);
            if (std::filesystem::exists(hint_path))
            {
                KIO_CO_TRY_LOG(co_await kio::AsyncUnlink(ctx, AT_FDCWD, hint_path, 0));
            }

            stats_.files_compacted_total++;
        }
        co_return bytes_reclaimed;
    }

    kio::Task<kio::Result<void>> Compactor::Compact(kio::IoContext& ctx)
    {
        auto partition_id = io_.PartitionID();

        if (compaction_running_.load(std::memory_order_acquire))
            co_return {};

        const auto fragmented_files = FindFragmentedFiles();
        if (fragmented_files.empty())
        {
            ALOG_DEBUG("Partition {}: no files to compact", partition_id);
            co_return {};
        }

        ALOG_INFO("Partition {}: compacting {} files", partition_id, fragmented_files.size());
        compaction_running_.store(true, std::memory_order_release);

        // 1. Setup Destination
        const uint64_t dst_file_id = io_.FdGen().Next();
        const auto dst_path = config_.directory / std::format("partition_{}/data_{}.db", partition_id, dst_file_id);
        // Error handling block for cleanup on failure
        auto run_compaction = [&]() -> kio::Task<kio::Result<void>>
        {
            auto dst_fd =
                KIO_CO_TRY_LOG(co_await kio::AsyncOpen(ctx, dst_path, config_.write_flags, config_.file_mode));

            auto shared_fd = std::make_shared<kio::FDGuard>(std::move(dst_fd));

            DataFile dst_file(shared_fd, dst_file_id, config_);

            // 2. Perform Compaction (Heavy I/O)
            const auto result = KIO_CO_TRY_LOG(co_await CompactFiles(ctx, fragmented_files, dst_file));

            // 3. Update Memory Index
            UpdateKeyDir(result.new_hints, dst_file_id);

            // 4. Delete Old Files
            uint64_t reclaimed = KIO_CO_TRY_LOG(co_await DeleteSourceFiles(ctx, fragmented_files));

            stats_.bytes_reclaimed_total += reclaimed;
            stats_.compactions_total++;

            ALOG_INFO("Partition {}: reclaimed {} bytes from {} files", partition_id, reclaimed,
                      fragmented_files.size());
            co_return {};
        };

        auto final_res = co_await run_compaction();

        if (!final_res)
        {
            ALOG_ERROR("Partition {}: compaction failed: {}", partition_id, final_res.error().message());
            stats_.compactions_failed++;
            // Try to clean up a partial file
            co_await kio::AsyncUnlink(ctx, AT_FDCWD, dst_path, 0);
        }

        compaction_running_.store(false, std::memory_order_release);
        co_return final_res;
    }

    std::vector<uint64_t> Compactor::FindFragmentedFiles() const
    {
        std::vector<uint64_t> res;
        const auto active_id = io_.ActiveFileID();

        for (const auto& [file_id, file_stats] : stats_.data_files)
        {
            if (active_id && *active_id == file_id)
            {
                continue;
            }

            if (file_stats.Fragmentation() >= config_.fragmentation_threshold)
            {
                res.push_back(file_id);
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

    kio::Task<> Compactor::CompactionLoop(kio::IoContext& ctx)
    {
        // TODO: Manage errors on async methods
        // We might want to handle the errors gracefully and
        // break the loop to signal compaction completion

        while (!shutting_down_.load(std::memory_order_acquire))
        {
            co_await Compact(ctx);

            // Wait for signal (or timeout after 60 seconds)
            co_await kio::AsyncSleep(ctx, config_.compaction_interval_s);
        }
    }
} // namespace bitcask
