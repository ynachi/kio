//
// Created by Yao ACHI on 07/02/2026.
//

#include "kio/logger.hpp"
#include "bitcask/partition.hpp"

namespace bitcask
{
    namespace fs = std::filesystem;

    Partition::Partition(const BitcaskConfig& config, const size_t partition_id)
        : fd_cache_(config.max_open_sealed_files),
          file_id_gen_(partition_id),
          config_(config),
          partition_id_(partition_id)
    {
    }

    kio::Task<kio::Result<void>> Partition::Put(kio::IoContext& ctx, std::string key, std::span<const std::byte> value)
    {
        if (active_file_->ShouldRotate(config_.max_file_size))
        {
            KIO_CO_TRY(co_await RotateActiveFile(ctx));
        }

        uint64_t const ts = GetCurrentTimestamp();
        uint32_t const total_size = kEntryFixedHeaderSize + key.size() + value.size();

        const auto offset = KIO_CO_TRY(co_await active_file_->AsyncWrite(ctx, key, value, ts, kFlagNone));

        const ValueLocation new_loc{
            .file_id = active_file_->FileId(), .offset = offset, .total_size = total_size, .timestamp_ns = ts
        };

        auto& dst_stats = stats_.data_files[new_loc.file_id];

        if (auto [it, inserted] = keydir_.try_emplace(key, new_loc); !inserted)
        {
            const auto& old_loc = it->second;
            auto& old_stats = stats_.data_files[old_loc.file_id];
            old_stats.live_bytes -= old_loc.total_size;
            old_stats.live_entries--;
            it->second = new_loc;
        }

        dst_stats.live_bytes += new_loc.total_size;
        dst_stats.live_entries++;
        dst_stats.total_bytes += new_loc.total_size;

        stats_.puts_total++;

        co_return {};
    }

    kio::Task<kio::Result<DataEntry>> Partition::AsyncReadEntry(kio::IoContext& ctx, const int fd,
                                                                const uint64_t offset, const uint32_t size) const
    {
        std::vector<std::byte> buffer(size);
        KIO_CO_TRY(co_await kio::AsyncReadExact(ctx, fd, buffer, offset));
        auto entry = KIO_CO_TRY(DataEntry::Deserialize(buffer));
        co_return entry;
    }

    kio::Task<kio::Result<std::optional<std::vector<std::byte>>>> Partition::Get(
        kio::IoContext& ctx, std::string_view key)
    {
        stats_.gets_total++;

        const auto it = keydir_.find(key);

        if (it == keydir_.end())
        {
            stats_.gets_miss_total++;
            co_return std::nullopt;
        }

        const auto& loc = it->second;

        // get the fd
        int fd{};
        if (active_file_ && loc.file_id == active_file_->FileId())
        {
            fd = active_file_->Fd();
        }
        else
        {
            const auto path = GetDataFilePath(loc.file_id);
            fd = KIO_CO_TRY(co_await fd_cache_.GetOrOpen(ctx, loc.file_id, path));
        }

        const DataEntry entry = KIO_CO_TRY(co_await AsyncReadEntry(ctx, fd, loc.offset, loc.total_size));

        if (entry.GetKeyView() != key)
        {
            ALOG_ERROR("CORRUPTION/LOGIC ERROR in Partition {}: Map points to key '{}' at {}:{}, but disk has '{}'",
                       partition_id_, key, loc.file_id, loc.offset, entry.GetKeyView());
            stats_.gets_miss_total++;
            co_return std::nullopt;
        }

        if (entry.IsTombstone())
        {
            stats_.gets_miss_total++;
            co_return std::nullopt;
        }

        co_return entry.GetValueOwned();
    }

    kio::Task<kio::Result<void>> Partition::Del(kio::IoContext& ctx, std::string key)
    {
        stats_.deletes_total++;

        const auto it = keydir_.find(key);
        if (it == keydir_.end())
        {
            co_return {};
        }

        auto& old_stats = stats_.data_files[it->second.file_id];
        old_stats.live_bytes -= it->second.total_size;
        old_stats.live_entries--;

        const uint64_t old_file_id = it->second.file_id;

        keydir_.erase(it);

        const DataEntry tombstone(std::string(key), {}, kFlagTombstone, GetCurrentTimestamp());

        KIO_CO_TRY(co_await active_file_->AsyncWrite(ctx, tombstone));

        auto& active_stats = stats_.data_files[active_file_->FileId()];
        active_stats.total_bytes += tombstone.Size();

        if (ShouldCompactFile(old_file_id))
        {
            SignalCompaction(old_file_id);
        }

        co_return {};
    }

    void Partition::SignalCompaction(const uint64_t file_id)
    {
        // Do not signal already tracked files
        if (compaction_candidates_.contains(file_id))
        {
            return;
        }
        compaction_candidates_.insert(file_id);
        compaction_signal_.Signal();
    }

    std::vector<uint64_t> Partition::ScanDataFiles() const
    {
        std::vector<uint64_t> ids;
        const fs::path p_dir = config_.directory / std::format("partition_{}", partition_id_);
        if (!fs::exists(p_dir)) return ids;
        for (const auto& entry : fs::directory_iterator(p_dir))
        {
            if (entry.path().extension() == ".db" && entry.path().stem().string().starts_with("data_"))
            {
                ids.push_back(std::stoull(entry.path().stem().string().substr(5)));
            }
        }
        std::ranges::sort(ids);
        return ids;
    }

    std::vector<uint64_t> Partition::FindFragmentedFiles() const
    {
        // Iterate only over candidates we specifically marked
        std::vector<uint64_t> res;
        for (const auto& fid : compaction_candidates_)
        {
            // Sanity check: ensure a file exists in stats and isn't active
            if (active_file_ && fid == active_file_->FileId()) continue;
            if (stats_.data_files.contains(fid))
            {
                res.push_back(fid);
            }
        }
        return res;
    }

    bool Partition::ShouldCompactFile(const uint64_t file_id) const
    {
        const auto it = stats_.data_files.find(file_id);
        if (it == stats_.data_files.end())
        {
            return false;
        }
        return it->second.Fragmentation() >= config_.fragmentation_threshold;
    }

    std::filesystem::path Partition::GetDataFilePath(uint64_t file_id) const
    {
        return config_.directory / std::format("partition_{}/data_{}.db", partition_id_, file_id);
    }

    kio::Task<kio::Result<void>> Partition::RotateActiveFile(kio::IoContext& ctx)
    {
        const uint64_t sealed_file_id = active_file_->FileId();
        const uint64_t actual_size = active_file_->Size();
        const int active_fd = active_file_->Fd();

        // Sync before sealing
        KIO_CO_TRY(co_await kio::AsyncFsync(ctx, active_fd));

        // Truncate to actual size (remove fallocate padding)
        KIO_CO_TRY(co_await kio::AsyncFtruncate(ctx, active_fd, static_cast<off_t>(actual_size)));

        KIO_CO_TRY(co_await WriteHintFile(ctx, sealed_file_id));

        KIO_CO_TRY(co_await kio::AsyncClose(ctx, active_fd));

        KIO_CO_TRY(co_await CreateAndSetActiveFile(ctx));

        stats_.file_rotations_total++;
        co_return {};
    }

    kio::Task<kio::Result<void>> Partition::CreateAndSetActiveFile(kio::IoContext& ctx)
    {
        uint64_t const new_id = file_id_gen_.Next();
        int const new_fd =
            KIO_CO_TRY(co_await kio::AsyncOpen(ctx, GetDataFilePath(new_id), config_.write_flags, config_.file_mode)).
            Get();

        active_file_ = std::make_unique<DataFile>(new_fd, new_id, config_);

        // Pre-allocate file space
        auto fallocate_result = co_await kio::AsyncFallocate(ctx, new_fd, 0, 0,
                                                             static_cast<off_t>(config_.max_file_size));
        if (!fallocate_result.has_value())
        {
            // Fallocate failure is non-fatal on some filesystems, just log
            ALOG_ERROR("Fallocate failed for file {}: {}", new_id, fallocate_result.error().message());
        }

        // Initialize stats for a new file
        stats_.data_files[new_id] = PartitionStats::FileStats{};

        co_return {};
    }

    kio::Task<kio::Result<void>> Partition::SealActiveFile(kio::IoContext& ctx)
    {
        // Check if there is an active file to seal and if it has content
        if (active_file_ == nullptr)
        {
            co_return {};
        }

        const uint64_t actual_size = active_file_->Size();
        const int active_fd = active_file_->Fd();
        const uint64_t sealed_file_id = ActiveFileId();

        if (active_fd >= 0 && actual_size > 0)
        {
            KIO_CO_TRY(co_await kio::AsyncFsync(ctx, active_fd));
            KIO_CO_TRY(co_await kio::AsyncFtruncate(ctx, active_fd, static_cast<off_t>(actual_size)));

            // Write a hint file for the sealed file
            KIO_CO_TRY(co_await WriteHintFile(ctx, sealed_file_id));

            KIO_CO_TRY(co_await kio::AsyncClose(ctx, active_fd));
        }
        else if (active_fd >= 0)
        {
            // Empty file - close and remove
            ALOG_DEBUG("The active file is empty, removing file_id {}", sealed_file_id);
            KIO_CO_TRY(co_await kio::AsyncClose(ctx, active_fd));

            const auto path = GetDataFilePath(sealed_file_id);
            auto unlink_result = co_await kio::AsyncUnlink(ctx, AT_FDCWD, path, 0);
            if (!unlink_result.has_value())
            {
                ALOG_ERROR("Failed to remove empty file {}: {}", sealed_file_id, unlink_result.error().message());
            }

            // Remove from stats
            stats_.data_files.erase(sealed_file_id);
        }

        // Remove pointer to prevent double-closing in dtor
        active_file_.reset();

        co_return {};
    }

    kio::Task<kio::Result<void>> Partition::WriteHintFile(kio::IoContext& ctx, uint64_t file_id)
    {
        const auto hint_path = GetHintFilePath(file_id);

        // Open a hint file for writing
        const auto hint_handle =
            KIO_CO_TRY(co_await kio::AsyncOpen(ctx, hint_path, O_CREAT | O_WRONLY | O_TRUNC, config_.file_mode));

        // Collect all entries for this file from the keydir
        std::vector<HintEntry> hints;
        for (const auto& [key, loc] : keydir_)
        {
            if (loc.file_id == file_id)
            {
                hints.emplace_back(loc.timestamp_ns, loc.offset, loc.total_size, std::string(key));
            }
        }

        // Write all hints using once; hint entries are not that large
        size_t total = 0;
        for (const auto& h : hints) total += h.Size();
        std::vector<std::byte> buf(total);

        size_t off = 0;
        for (const auto& h : hints)
        {
            std::span dst(buf.data() + off, buf.size() - off);
            const auto written = h.SerializeTo(dst);
            off += written;
        }

        if (!buf.empty())
        {
            KIO_CO_TRY(co_await kio::AsyncWriteExact(ctx, hint_handle, buf));
        }

        KIO_CO_TRY(co_await kio::AsyncFsync(ctx, hint_handle));

        ALOG_DEBUG("Wrote hint file for file_id {} with {} entries", file_id, hints.size());
        co_return {};
    }

    kio::Task<kio::Result<void>> Partition::Compact(kio::IoContext& ctx)
    {
        if (compaction_running_.load(std::memory_order_acquire))
        {
            co_return {};
        }

        const auto fragmented_files = FindFragmentedFiles();

        if (fragmented_files.empty())
        {
            ALOG_DEBUG("Partition {}: no files to compact", partition_id_);
            co_return {};
        }

        ALOG_INFO("Partition {}: compacting {} files", partition_id_, fragmented_files.size());

        compaction_running_.store(true, std::memory_order_release);

        // Generate destination file ID
        const uint64_t dst_file_id = file_id_gen_.Next();
        const auto dst_path = config_.directory / std::format("partition_{}/data_{}.db", partition_id_, dst_file_id);
        const int dst_fd = KIO_CO_TRY(co_await kio::AsyncOpen(ctx, dst_path, config_.write_flags, config_.file_mode)).
            Get();

        DataFile dst_file(dst_fd, dst_file_id, config_);

        const auto result = co_await CompactFiles(ctx, fragmented_files, dst_file);

        if (!result.has_value())
        {
            ALOG_ERROR("Partition {}: compaction failed: {}", partition_id_, result.error().message());
            stats_.compactions_failed++;
            compaction_running_.store(false, std::memory_order_release);

            // Clean up the partial destination file
            KIO_CO_TRY(co_await AsyncUnlink(ctx, AT_FDCWD, GetDataFilePath(dst_file_id), 0));
            // TODO fix this error category
            co_return std::unexpected(std::error_code());
        }

        for (const auto& hint : result->new_hints)
        {
            if (auto it = keydir_.find(hint.key); it != keydir_.end())
            {
                it->second.file_id = dst_file_id;
                it->second.offset = hint.offset;
                it->second.total_size = hint.size;
                it->second.timestamp_ns = hint.timestamp_ns;
            }
        }

        ALOG_INFO("Partition {}: compaction succeeded, cleaning up {} source files", partition_id_,
                  fragmented_files.size());
        stats_.compactions_total++;

        // Delete source files after successful compaction
        uint64_t bytes_reclaimed = 0;
        for (const uint64_t src_id : fragmented_files)
        {
            // Remove from FD cache first
            co_await fd_cache_.Remove(ctx, src_id);

            // Track reclaimed bytes
            if (auto it = stats_.data_files.find(src_id); it != stats_.data_files.end())
            {
                bytes_reclaimed += it->second.total_bytes;
                stats_.data_files.erase(it);
            }

            // Delete data file
            auto data_path = GetDataFilePath(src_id);
            KIO_CO_TRY(co_await kio::AsyncUnlink(ctx, AT_FDCWD, data_path, 0));

            // Delete a hint file if exists
            if (auto hint_path = GetHintFilePath(src_id); std::filesystem::exists(hint_path))
            {
                KIO_CO_TRY(co_await kio::AsyncUnlink(ctx, AT_FDCWD, hint_path, 0));
            }

            stats_.files_compacted_total++;
        }

        stats_.bytes_reclaimed_total += bytes_reclaimed;
        stats_.compaction_running = false;

        ALOG_INFO("Partition {}: reclaimed {} bytes from {} files", partition_id_, bytes_reclaimed,
                  fragmented_files.size());

        co_return {};
    }

    kio::Task<kio::Result<CompactionResult>> Partition::CompactFiles(kio::IoContext& ctx,
                                                                     const std::vector<uint64_t>& src_file_ids,
                                                                     DataFile& dst_file)
    {
        CompactionResult result;

        kio::IoBuffer in_buf(kDefaultInBufReadSize);
        kio::IoBuffer out_buf(kDefaultOutBufWriteSize);

        // Track where we are writing in the destination file
        uint64_t dst_write_offset = dst_file.Size();

        // We track the logical offset in the destination file for the *next* entry to be written.
        // This includes data currently sitting in out_buf.
        uint64_t dst_logical_offset = dst_write_offset;

        for (const uint64_t src_id : src_file_ids)
        {
            const auto src_path = config_.directory / std::format("partition_{}/data_{}.db", partition_id_, src_id);
            int src_fd = KIO_CO_TRY(co_await fd_cache_.GetOrOpen(ctx, src_id, src_path));

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
                    if (auto it = keydir_.find(key_view); it != keydir_.end())
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
            dst_write_offset += out_span.size();
            out_buf.Clear();
        }

        ALOG_DEBUG("Compaction: Syncing dst file (fd={})", dst_file.Fd());
        KIO_CO_TRY(co_await kio::AsyncFdatasync(ctx, dst_file.Fd()));

        co_return result;
    }
}
