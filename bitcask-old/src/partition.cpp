#include "bitcask/include/partition.h"

#include <algorithm>
#include <memory>

#include <kio/core/bytes_mut.h>

using namespace bitcask;
using namespace kio;
using namespace kio::io;

Partition::Partition(const BitcaskConfig& config, Worker& worker, const size_t partition_id)
    : worker_(worker),
      fd_cache_(worker, config.max_open_sealed_files),
      file_id_gen_(partition_id),
      config_(config),
      partition_id_(partition_id)
{
    //  Recovery, compaction loop delegated to open factory method
}

Task<Result<void>> Partition::Put(std::string&& key, std::vector<char>&& value)
{
    // Assert partition is initialized
    assert(active_file_ && "Partition not initialized - active_file_ is null");

    co_await SwitchToWorker(worker_);

    if (active_file_->ShouldRotate(config_.max_file_size))
    {
        KIO_TRY(co_await RotateActiveFile());
    }

    uint64_t const ts = GetCurrentTimestamp();
    uint32_t const total_size = kEntryFixedHeaderSize + key.size() + value.size();

    const auto offset = KIO_TRY(co_await active_file_->AsyncWrite(key, value, ts, kFlagNone));

    const ValueLocation new_loc{
        .file_id = active_file_->FileId(), .offset = offset, .total_size = total_size, .timestamp_ns = ts};

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

Task<Result<std::optional<std::vector<char>>>> Partition::Get(const std::string_view key)
{
    // Assert partition is initialized
    assert(active_file_ && "Partition not initialized - active_file_ is null");

    co_await SwitchToWorker(worker_);
    stats_.gets_total++;

    const auto it = keydir_.find(key);

    if (it == keydir_.end())
    {
        stats_.gets_miss_total++;
        co_return std::nullopt;
    }

    const auto& loc = it->second;
    const int fd = KIO_TRY(co_await FindFd(loc.file_id));

    ALOG_TRACE("Reading entry from file {} at offset {} with size {}", loc.file_id, loc.offset, loc.total_size);
    const DataEntry entry = KIO_TRY(co_await AsyncReadEntry(fd, loc.offset, loc.total_size));

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

Task<Result<void>> Partition::Del(const std::string_view key)
{
    assert(active_file_);

    co_await SwitchToWorker(worker_);
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

    const DataEntry tombstone(std::string(key), std::vector<char>{}, kFlagTombstone, GetCurrentTimestamp());

    KIO_TRY(co_await active_file_->AsyncWrite(tombstone));

    auto& active_stats = stats_.data_files[active_file_->FileId()];
    active_stats.total_bytes += tombstone.Size();

    if (ShouldCompactFile(old_file_id))
    {
        SignalCompaction(old_file_id);
    }

    co_return {};
}

Task<Result<int>> Partition::FindFd(const uint64_t file_id)
{
    if (file_id == active_file_->FileId())
    {
        co_return active_file_->Handle().Get();
    }

    const auto path = GetDataFilePath(file_id);
    co_return co_await fd_cache_.GetOrOpen(file_id, path);
}

Task<Result<DataEntry>> Partition::AsyncReadEntry(const int fd, const uint64_t offset, const uint32_t size) const
{
    std::vector<char> buffer(size);
    KIO_TRY(co_await worker_.AsyncReadExactAt(fd, buffer, offset));
    auto entry = KIO_TRY(DataEntry::Deserialize(buffer));
    co_return entry;
}

Task<Result<void>> Partition::RotateActiveFile()
{
    assert(active_file_);

    const uint64_t sealed_file_id = active_file_->FileId();
    const uint64_t actual_size = active_file_->Size();
    const int active_fd = active_file_->Handle().Get();

    // Sync before sealing
    KIO_TRY(co_await worker_.AsyncFsync(active_fd));

    // Truncate to actual size (remove fallocate padding)
    KIO_TRY(co_await worker_.AsyncFtruncate(active_fd, static_cast<off_t>(actual_size)));

    KIO_TRY(co_await WriteHintFile(sealed_file_id));

    KIO_TRY(co_await active_file_->AsyncClose());

    KIO_TRY(co_await CreateAndSetActiveFile());

    stats_.file_rotations_total++;
    co_return {};
}

// FIX #6: New method to write hint file when sealing a data file
Task<Result<void>> Partition::WriteHintFile(uint64_t file_id)
{
    const auto hint_path = GetHintFilePath(file_id);

    // Open hint file for writing
    const int hint_fd =
        KIO_TRY(co_await worker_.AsyncOpenAt(hint_path, O_CREAT | O_WRONLY | O_TRUNC, config_.file_mode));

    FileHandle hint_handle(hint_fd);

    // Collect all entries for this file from keydir
    std::vector<HintEntry> hints;
    for (const auto& [key, loc] : keydir_)
    {
        if (loc.file_id == file_id)
        {
            hints.emplace_back(loc.timestamp_ns, loc.offset, loc.total_size, std::string(key));
        }
    }

    // Write all hints using vectored I/O for efficiency
    for (const auto& hint : hints)
    {
        auto serialized = hint.Serialize();
        KIO_TRY(co_await worker_.AsyncWriteExact(hint_fd, serialized));
    }

    KIO_TRY(co_await worker_.AsyncFsync(hint_fd));

    ALOG_DEBUG("Wrote hint file for file_id {} with {} entries", file_id, hints.size());
    co_return {};
}

Task<Result<void>> Partition::CreateAndSetActiveFile()
{
    uint64_t const new_id = file_id_gen_.Next();
    int const new_fd =
        KIO_TRY(co_await worker_.AsyncOpenAt(GetDataFilePath(new_id), config_.write_flags, config_.file_mode));

    active_file_ = std::make_unique<DataFile>(new_fd, new_id, worker_, config_);

    // Pre-allocate file space
    auto fallocate_result = co_await worker_.AsyncFallocate(new_fd, 0, static_cast<off_t>(config_.max_file_size));
    if (!fallocate_result.has_value())
    {
        // Fallocate failure is non-fatal on some filesystems, just log
        ALOG_WARN("Fallocate failed for file {}: {}", new_id, fallocate_result.error());
    }

    // Initialize stats for new file
    stats_.data_files[new_id] = PartitionStats::FileStats{};

    co_return {};
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

void Partition::SignalCompaction(const uint64_t file_id)
{
    if (ShouldCompactFile(file_id))
    {
        compaction_trigger_.Post();
    }
}

std::vector<uint64_t> Partition::FindFragmentedFiles() const
{
    std::vector<uint64_t> result;

    for (const auto& [file_id, file_stats] : stats_.data_files)
    {
        // Never compact the active file
        if (file_id == active_file_->FileId())
        {
            continue;
        }

        if (file_stats.Fragmentation() >= config_.fragmentation_threshold)
        {
            result.push_back(file_id);
        }
    }

    return result;
}

std::filesystem::path Partition::GetDataFilePath(uint64_t file_id) const
{
    return config_.directory / std::format("partition_{}/data_{}.db", partition_id_, file_id);
}

std::filesystem::path Partition::GetHintFilePath(uint64_t file_id) const
{
    return config_.directory / std::format("partition_{}/hint_{}.ht", partition_id_, file_id);
}

std::vector<uint64_t> Partition::ScanDataFiles() const
{
    const std::filesystem::path dir_to_scan = DbPath() / std::format("partition_{}", partition_id_);

    if (!std::filesystem::exists(dir_to_scan) || !std::filesystem::is_directory(dir_to_scan))
    {
        return {};
    }

    std::vector<uint64_t> result;
    constexpr size_t prefix_len = kDataFilePrefix.size();
    constexpr size_t ext_len = kDataFileExtension.size();

    for (const auto& dir_entry : std::filesystem::directory_iterator(dir_to_scan))
    {
        if (!dir_entry.is_regular_file())
        {
            continue;
        }

        const auto filename = dir_entry.path().filename().string();

        // Check prefix and extension
        if (filename.size() <= prefix_len + ext_len)
        {
            continue;
        }

        if (!filename.starts_with(kDataFilePrefix) || !filename.ends_with(kDataFileExtension))
        {
            continue;
        }

        // Extract file ID from filename: data_<id>.db
        const auto id_str = filename.substr(prefix_len, filename.size() - prefix_len - ext_len);

        try
        {
            uint64_t const file_id = std::stoull(id_str);
            result.push_back(file_id);
        }
        catch (const std::exception& e)
        {
            ALOG_WARN("Skipping file with unparseable ID: {}", filename);
            continue;
        }
    }

    // Sort by file ID (which embeds timestamp) for the correct replay order
    std::ranges::sort(result, FileIdCompareByTime);

    return result;
}

DetachedTask Partition::CompactionLoop()
{
    co_await SwitchToWorker(worker_);
    ALOG_INFO("Partition {} compaction loop starting", partition_id_);

    const auto st = worker_.GetStopToken();
    while (!st.stop_requested() && !shutting_down_.load())
    {
        co_await compaction_trigger_.WaitFor(worker_, config_.compaction_interval_s);
        if (shutting_down_.load())
        {
            break;
        }

        if (auto res = co_await Compact(); !res.has_value())
        {
            ALOG_ERROR("Partition {}: co_await compaction failed", partition_id_);
        }
        compaction_trigger_.Reset();
    }
    ALOG_INFO("Partition {} compaction loop exiting", partition_id_);

    compaction_stop_.Post();
}

Result<std::pair<uint64_t, uint64_t>> Partition::RecoverDataFromBuffer(BytesMut& buffer, const uint64_t file_id,
                                                                       const uint64_t file_read_position)
{
    uint64_t entries_recovered = 0;
    uint64_t entries_skipped = 0;

    while (buffer.Remaining() >= kEntryFixedHeaderSize)
    {
        const auto readable = buffer.ReadableSpan();
        auto entry_result = DataEntry::Deserialize(readable);

        if (!entry_result.has_value())
        {
            if (entry_result.error().value == kIoNeedMoreData)
            {
                // Need more data - break and let caller read more
                break;
            }
            // Actual corruption - could truncate here for crash recovery
            return std::unexpected(entry_result.error());
        }

        auto entry = std::move(entry_result.value());

        // file_read_position = total bytes read from file so far
        // buffer.Remaining() = unprocessed bytes still in buffer
        // Therefore, the start of current readable data in the file is:
        const uint64_t entry_offset = file_read_position - buffer.Remaining();

        buffer.Advance(entry.Size());

        if (entry.IsTombstone())
        {
            keydir_.erase(entry.GetKeyView());
            entries_skipped++;
            continue;
        }

        entries_recovered++;

        // Use insert_or_assign with proper key (need to copy since entry will be destroyed)
        std::string key_copy(entry.GetKeyView());
        keydir_.insert_or_assign(std::move(key_copy), ValueLocation{.file_id = file_id,
                                                                    .offset = entry_offset,
                                                                    .total_size = entry.Size(),
                                                                    .timestamp_ns = entry.GetTimestamp()});
    }
    return std::make_pair(entries_recovered, entries_skipped);
}

Task<Result<void>> Partition::RecoverFromDataFile(const FileHandle& fh, uint64_t file_id)
{
    const int fd = fh.Get();
    const uint64_t file_size = KIO_TRY(GetFileSize(fd));

    if (file_size == 0)
    {
        co_return {};
    }

    BytesMut buffer(config_.read_buffer_size);
    uint64_t file_read_position = 0;
    uint64_t total_recovered = 0;
    uint64_t total_skipped = 0;

    while (file_read_position < file_size)
    {
        // Ensure we have space to read
        buffer.Reserve(config_.read_buffer_size);
        auto writable = buffer.WritableSpan();

        const uint64_t bytes_to_read = std::min(writable.size(), file_size - file_read_position);
        auto read_result = co_await worker_.AsyncReadAt(fd, writable.subspan(0, bytes_to_read), file_read_position);

        if (!read_result.has_value())
        {
            ALOG_ERROR("Read error during recovery of file {}: {}", file_id, read_result.error());
            co_return std::unexpected(read_result.error());
        }

        if (read_result.value() == 0)
        {
            break;  // EOF
        }

        buffer.CommitWrite(read_result.value());
        file_read_position += read_result.value();

        // Process entries in buffer
        auto process_result = RecoverDataFromBuffer(buffer, file_id, file_read_position);
        if (!process_result.has_value())
        {
            // If we hit corruption, we could choose to truncate and continue
            // For now, we log and stop processing this file
            ALOG_ERROR("Corruption detected in file {} during recovery: {}", file_id, process_result.error());
            break;
        }

        auto [recovered, skipped] = process_result.value();
        total_recovered += recovered;
        total_skipped += skipped;

        // Compact buffer if needed
        if (buffer.ShouldCompact())
        {
            buffer.Compact();
        }
    }

    ALOG_INFO("Recovered {} entries ({} tombstones) from data file {}", total_recovered, total_skipped, file_id);
    co_return {};
}

Task<Result<void>> Partition::RecoverFromHintFile(const FileHandle& fh, uint64_t file_id)
{
    const int fd = fh.Get();
    const uint64_t file_size = KIO_TRY(GetFileSize(fd));

    if (file_size == 0)
    {
        co_return {};
    }

    // Hint files are small, read entirely
    std::vector<char> buffer(file_size);
    KIO_TRY(co_await worker_.AsyncReadExactAt(fd, buffer, 0));

    std::span<const char> remaining(buffer);
    uint64_t entries_recovered = 0;

    while (remaining.size() >= kHintHeaderSize)
    {
        auto result = HintEntry::Deserialize(remaining);
        if (!result.has_value())
        {
            if (result.error().value == kIoNeedMoreData)
            {
                break;  // Truncated hint file
            }
            co_return std::unexpected(result.error());
        }

        auto [hint, consumed] = result.value();
        remaining = remaining.subspan(consumed);

        // Insert into keydir (hint entries are always live - tombstones aren't in hints)
        keydir_.insert_or_assign(
            std::move(hint.key),
            ValueLocation{
                .file_id = file_id, .offset = hint.offset, .total_size = hint.size, .timestamp_ns = hint.timestamp_ns});
        entries_recovered++;
    }

    ALOG_INFO("Recovered {} entries from hint file {}", entries_recovered, file_id);
    co_return {};
}

Task<Result<void>> Partition::Recover()
{
    const auto files = ScanDataFiles();

    // Initialize file stats with on-disk sizes
    for (auto file_id : files)
    {
        const auto path = GetDataFilePath(file_id);
        if (std::filesystem::exists(path))
        {
            stats_.data_files[file_id].total_bytes = std::filesystem::file_size(path);
        }
    }

    uint64_t max_file_id = 0;
    for (const auto file_id : files)
    {
        max_file_id = std::max(file_id, max_file_id);

        // Try hint file first (faster)
        if (const auto hint_result = co_await TryRecoverFromHint(file_id); hint_result)
        {
            continue;
        }

        // Fall back to full data file scan
        const auto fd =
            KIO_TRY(co_await worker_.AsyncOpenAt(GetDataFilePath(file_id), config_.read_flags, config_.file_mode));
        FileHandle const data(fd);
        if (auto res = co_await RecoverFromDataFile(data, file_id); !res.has_value())
        {
            ALOG_ERROR("Failed to recover data file {}: {}", file_id, res.error());
            // Continue with other files
        }
    }

    // Compute live stats from keydir
    for (const auto& loc : keydir_ | std::views::values)
    {
        auto& fs = stats_.data_files[loc.file_id];
        fs.live_bytes += loc.total_size;
        fs.live_entries++;
    }

    // Update file ID generator to ensure monotonicity
    if (max_file_id > 0)
    {
        const auto decoded = FileID::Decode(max_file_id);
        file_id_gen_.UpdateState(decoded.timestamp_sec, decoded.sequence);
    }

    KIO_TRY(co_await CreateAndSetActiveFile());
    co_return {};
}

// Helper to try hint file recovery
Task<bool> Partition::TryRecoverFromHint(uint64_t file_id)
{
    const auto hint_path = GetHintFilePath(file_id);
    if (!std::filesystem::exists(hint_path))
    {
        ALOG_DEBUG("Hint file {} does not exist", file_id);
        co_return false;
    }

    const auto fd = co_await worker_.AsyncOpenAt(hint_path, config_.read_flags, config_.file_mode);
    if (!fd.has_value())
    {
        ALOG_ERROR("Failed to open hint file {}", file_id);
        co_return false;
    }

    const FileHandle hint(fd.value());
    const auto result = co_await RecoverFromHintFile(hint, file_id);
    if (!result.has_value())
    {
        ALOG_ERROR("Failed to recover hint file {}", file_id);
    }
    co_return result.has_value();
}

Task<Result<std::unique_ptr<Partition>>> Partition::Open(const BitcaskConfig& config, Worker& worker,
                                                         const size_t partition_id)
{
    ALOG_INFO("Opening partition {}", partition_id);
    auto partition = std::make_unique<Partition>(config, worker, partition_id);
    KIO_TRY(co_await partition->Recover());
    if (config.auto_compact)
    {
        // run the compaction loop in the background
        partition->CompactionLoop();
    }
    // if durability not set, start periodic sync
    if (!config.sync_on_write)
    {
        partition->BackgroundSync();
    }
    co_return std::move(partition);
}

Task<Result<void>> Partition::SealActiveFile()
{
    co_await SwitchToWorker(worker_);

    // Check if there is an active file to seal and if it has content
    if (active_file_ == nullptr)
    {
        co_return {};
    }

    const uint64_t actual_size = active_file_->Size();
    const int active_fd = active_file_->Handle().Get();
    const uint64_t sealed_file_id = ActiveFileId();

    // FIX #8: fd 0 is valid (though unlikely), use >= 0
    if (active_fd >= 0 && actual_size > 0)
    {
        KIO_TRY(co_await worker_.AsyncFsync(active_fd));
        KIO_TRY(co_await worker_.AsyncFtruncate(active_fd, static_cast<off_t>(actual_size)));

        // Write hint file for the sealed file
        KIO_TRY(co_await WriteHintFile(sealed_file_id));

        KIO_TRY(co_await active_file_->AsyncClose());
    }
    else if (active_fd >= 0)
    {
        // Empty file - close and remove
        ALOG_DEBUG("The active file is empty, removing file_id {}", sealed_file_id);
        KIO_TRY(co_await active_file_->AsyncClose());

        const auto path = GetDataFilePath(sealed_file_id);
        auto unlink_result = co_await worker_.AsyncUnlinkAt(AT_FDCWD, path, 0);
        if (!unlink_result.has_value())
        {
            ALOG_WARN("Failed to remove empty file {}: {}", sealed_file_id, unlink_result.error());
        }

        // Remove from stats
        stats_.data_files.erase(sealed_file_id);
    }

    // Remove pointer to prevent double-closing in dtor
    active_file_.reset();

    co_return {};
}

Task<Result<void>> Partition::AsyncClose()
{
    ALOG_DEBUG("Partition {} closing...", partition_id_);

    // Signal shutdown to compaction loop
    shutting_down_.store(true);

    // Wake up the compaction loop so it can exit cleanly
    compaction_trigger_.Post();

    // Seal the active file if it exists and hasn't been sealed by rotation
    KIO_TRY(co_await SealActiveFile());

    // Clear FD cache
    fd_cache_.Clear();

    // wait for compaction to finish to avoid race condition
    co_await compaction_trigger_.Wait(worker_);

    // if background sync, wait for it
    if (!config_.sync_on_write)
    {
        co_await sync_job_stop_.Wait(worker_);
    }

    // The worker is not owned by the partition. It should not close it.

    ALOG_DEBUG("Partition {} closed successfully.", partition_id_);
    co_return {};
}

Task<Result<void>> Partition::Compact()
{
    if (stats_.compaction_running)
    {
        co_return {};
    }

    auto fragmented_files = FindFragmentedFiles();

    if (fragmented_files.empty())
    {
        ALOG_DEBUG("Partition {}: no files to compact", partition_id_);
        compaction_trigger_.Reset();
        co_return {};
    }

    ALOG_INFO("Partition {}: compacting {} files", partition_id_, fragmented_files.size());

    stats_.compaction_running = true;

    // Generate destination file ID
    const uint64_t dst_file_id = file_id_gen_.Next();

    // Create compaction context
    compactor::CompactionContext ctx(worker_, config_, keydir_, stats_, partition_id_, dst_file_id,
                                     std::move(fragmented_files), compaction_limits_);

    if (auto result = co_await compactor::CompactFiles(ctx); !result.has_value())
    {
        ALOG_ERROR("Partition {}: compaction failed: {}", partition_id_, result.error());
        stats_.compactions_failed++;
        stats_.compaction_running = false;

        // Clean up the partial destination file
        if (auto unlink_result = co_await worker_.AsyncUnlinkAt(AT_FDCWD, GetDataFilePath(dst_file_id), 0);
            !unlink_result.has_value())
        {
            ALOG_WARN("Failed to clean up partial compaction file: {}", unlink_result.error());
        }

        co_return std::unexpected(result.error());
    }

    ALOG_INFO("Partition {}: compaction succeeded, cleaning up {} source files", partition_id_,
              ctx.src_file_ids.size());
    stats_.compactions_total++;

    // Delete source files after successful compaction
    uint64_t bytes_reclaimed = 0;
    for (const uint64_t src_id : ctx.src_file_ids)
    {
        // Remove from FD cache first
        fd_cache_.Remove(src_id);

        // Track reclaimed bytes
        if (auto it = stats_.data_files.find(src_id); it != stats_.data_files.end())
        {
            bytes_reclaimed += it->second.total_bytes;
            stats_.data_files.erase(it);
        }

        // Delete data file
        auto data_path = GetDataFilePath(src_id);
        auto unlink_result = co_await worker_.AsyncUnlinkAt(AT_FDCWD, data_path, 0);
        if (!unlink_result.has_value())
        {
            ALOG_WARN("Failed to delete compacted data file {}: {}", src_id, unlink_result.error());
        }

        // Delete hint file if exists
        if (auto hint_path = GetHintFilePath(src_id); std::filesystem::exists(hint_path))
        {
            if (auto hint_unlink = co_await worker_.AsyncUnlinkAt(AT_FDCWD, hint_path, 0); !hint_unlink.has_value())
            {
                ALOG_WARN("Failed to delete hint file {}: {}", src_id, hint_unlink.error());
            }
        }

        stats_.files_compacted_total++;
    }

    stats_.bytes_reclaimed_total += bytes_reclaimed;
    stats_.compaction_running = false;

    ALOG_INFO("Partition {}: reclaimed {} bytes from {} files", partition_id_, bytes_reclaimed,
              ctx.src_file_ids.size());

    co_return {};
}

Task<Result<void>> Partition::Sync() const
{
    co_await SwitchToWorker(worker_);
    if (active_file_)
    {
        KIO_TRY(co_await worker_.AsyncFsync(active_file_->Handle().Get()));
    }
    co_return {};
}

DetachedTask Partition::BackgroundSync()
{
    co_await SwitchToWorker(worker_);
    ALOG_INFO("Partition {} background sync loop starting", partition_id_);

    const auto st = worker_.GetStopToken();
    while (!st.stop_requested() && !shutting_down_.load())
    {
        co_await worker_.AsyncSleep(std::chrono::milliseconds(config_.sync_interval));
        // check again after wakeup
        if (shutting_down_.load())
        {
            break;
        }

        if (auto res = co_await Sync(); !res.has_value())
        {
            ALOG_ERROR("Partition {}: failed to sync: {}", partition_id_, res.error());
            // do not exit
        }
    }
    ALOG_INFO("Partition {} background sync loop exiting", partition_id_);

    // signal stopped
    sync_job_stop_.Post();
}