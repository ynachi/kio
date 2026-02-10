#include "bitcask/partition_io.hpp"

#include <ranges>

namespace bitcask
{
    namespace fs = std::filesystem;

    namespace
    {
        kio::Task<kio::Result<DataEntry>> AsyncReadEntry(kio::IoContext& ctx, const int fd, const uint64_t offset,
                                                         const uint32_t size)
        {
            std::vector<std::byte> buffer(size);
            KIO_CO_TRY_LOG(co_await kio::AsyncReadExact(ctx, fd, buffer, offset));
            auto entry = KIO_CO_TRY_LOG(DataEntry::Deserialize(buffer));
            co_return entry;
        }
    } // namespace

    kio::Task<kio::Result<void>> PartitionIO::Put(kio::IoContext& ctx, std::string key,
                                                  std::span<const std::byte> value)
    {
        if (active_file_->ShouldRotate(config_.max_file_size))
        {
            KIO_CO_TRY_LOG(co_await RotateActiveFile(ctx));
        }

        uint64_t const ts = GetCurrentTimestamp();
        uint32_t const total_size = kEntryFixedHeaderSize + key.size() + value.size();

        const auto offset = KIO_CO_TRY_LOG(co_await active_file_->AsyncWrite(ctx, key, value, ts, kFlagNone));

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

    kio::Task<kio::Result<std::optional<std::vector<std::byte>>>> PartitionIO::Get(kio::IoContext& ctx,
        std::string_view key)
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
        SharedFD fd{};
        if (active_file_ && loc.file_id == active_file_->FileId())
        {
            fd = active_file_->Fd();
        }
        else
        {
            const auto path = GetDataFilePath(loc.file_id);
            fd = KIO_CO_TRY_LOG(co_await fd_cache_.GetOrOpen(ctx, loc.file_id, path));
        }

        const DataEntry entry = KIO_CO_TRY_LOG(co_await AsyncReadEntry(ctx, fd->Get(), loc.offset, loc.total_size));

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

    kio::Task<kio::Result<void>> PartitionIO::Del(kio::IoContext& ctx, std::string key)
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

        KIO_CO_TRY_LOG(co_await active_file_->AsyncWrite(ctx, tombstone));

        auto& active_stats = stats_.data_files[active_file_->FileId()];
        active_stats.total_bytes += tombstone.Size();

        co_return {};
    }

    kio::Task<kio::Result<void>> PartitionIO::RotateActiveFile(kio::IoContext& ctx)
    {
        const uint64_t sealed_file_id = active_file_->FileId();
        const uint64_t actual_size = active_file_->Size();
        auto active_fd_handle = active_file_->Fd();
        const int active_fd = active_fd_handle->Get();

        // Sync before sealing
        KIO_CO_TRY_LOG(co_await kio::AsyncFsync(ctx, active_fd));

        // Truncate to actual size (remove fallocate padding)
        KIO_CO_TRY_LOG(co_await kio::AsyncFtruncate(ctx, active_fd, static_cast<off_t>(actual_size)));

        KIO_CO_TRY_LOG(co_await WriteHintFile(ctx, sealed_file_id));
        const int fd_to_close = active_fd_handle->Release();
        KIO_CO_TRY_LOG(co_await kio::AsyncClose(ctx, fd_to_close));

        KIO_CO_TRY_LOG(co_await CreateAndSetActiveFile(ctx));

        stats_.file_rotations_total++;
        co_return {};
    }

    kio::Task<kio::Result<void>> PartitionIO::CreateAndSetActiveFile(kio::IoContext& ctx)
    {
        uint64_t const new_fid = file_id_gen_.Next();
        auto fd_guard =
            KIO_CO_TRY_LOG(
                co_await kio::AsyncOpen(ctx, GetDataFilePath(new_fid), config_.write_flags, config_.file_mode));

        auto shared_fd = std::make_shared<kio::FDGuard>(std::move(fd_guard));
        active_file_ = std::make_unique<DataFile>(shared_fd, new_fid, config_);

        // Pre-allocate file space
        auto fallocate_result = co_await kio::AsyncFallocate(ctx, shared_fd->Get(), 0, 0,
                                                             static_cast<off_t>(config_.max_file_size));
        if (!fallocate_result.has_value())
        {
            // Fallocate failure is non-fatal on some filesystems, just log
            ALOG_ERROR("Fallocate failed for file {}: {}", new_fid, fallocate_result.error().message());
        }

        // Initialize stats for a new file
        stats_.data_files[new_fid] = PartitionStats::FileStats{};

        co_return {};
    }

    kio::Task<kio::Result<void>> PartitionIO::SealActiveFile(kio::IoContext& ctx)
    {
        // Check if there is an active file to seal and if it has content
        if (active_file_ == nullptr)
        {
            co_return {};
        }

        const uint64_t actual_size = active_file_->Size();
        auto active_fd_handle = active_file_->Fd();
        const int active_fd = active_fd_handle->Get();
        const uint64_t sealed_file_id = ActiveFileId();

        if (active_fd >= 0 && actual_size > 0)
        {
            KIO_CO_TRY_LOG(co_await kio::AsyncFsync(ctx, active_fd));
            KIO_CO_TRY_LOG(co_await kio::AsyncFtruncate(ctx, active_fd, static_cast<off_t>(actual_size)));

            // Write a hint file for the sealed file
            KIO_CO_TRY_LOG(co_await WriteHintFile(ctx, sealed_file_id));

            const int fd_to_close = active_fd_handle->Release();
            KIO_CO_TRY_LOG(co_await kio::AsyncClose(ctx, fd_to_close));
        }
        else if (active_fd >= 0)
        {
            // Empty file - close and remove
            ALOG_DEBUG("The active file is empty, removing file_id {}", sealed_file_id);
            const int fd_to_close = active_fd_handle->Release();
            KIO_CO_TRY_LOG(co_await kio::AsyncClose(ctx, fd_to_close));

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

    std::vector<uint64_t> PartitionIO::ScanDataFiles() const
    {
        std::vector<uint64_t> ids;
        const fs::path p_dir = config_.directory / std::format("partition_{}", partition_id_);
        if (!fs::exists(p_dir))
            return ids;
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

    kio::Task<kio::Result<void>> PartitionIO::WriteHintFile(kio::IoContext& ctx, uint64_t file_id)
    {
        const auto hint_path = GetHintFilePath(file_id);

        // Open a hint file for writing
        const auto hint_handle =
            KIO_CO_TRY_LOG(co_await kio::AsyncOpen(ctx, hint_path, O_CREAT | O_WRONLY | O_TRUNC, config_.file_mode));

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
        for (const auto& h : hints)
            total += h.Size();
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
            KIO_CO_TRY_LOG(co_await kio::AsyncWriteExact(ctx, hint_handle, buf));
        }

        KIO_CO_TRY_LOG(co_await kio::AsyncFsync(ctx, hint_handle));

        ALOG_DEBUG("Wrote hint file for file_id {} with {} entries", file_id, hints.size());
        co_return {};
    }

    kio::Task<> PartitionIO::BackgroundSync(kio::IoContext& ctx)
    {
        while (!shutting_down_.load(std::memory_order_acquire))
        {
            if (auto res = co_await kio::AsyncSleep(ctx, std::chrono::milliseconds(config_.sync_interval));
                !res.has_value())
            {
                ALOG_ERROR("Background sync job failed to sleep: {}", res.error().message());
            }
            // check again after wakeup
            if (shutting_down_.load())
            {
                break;
            }

            if (active_file_ != nullptr)
            {
                if (auto res = co_await kio::AsyncFsync(ctx, active_file_->RawFd()); !res.has_value())
                {
                    ALOG_ERROR("Background sync job failed to sync file {}: {}", active_file_->FileId(),
                               res.error().message());
                }
            }
        }
        ALOG_INFO("Partition {} background sync loop exiting", partition_id_);

        co_return;
    }
} // namespace bitcask
