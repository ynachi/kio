#include "bitcask/include/partition.h"

#include <algorithm>
#include <memory>

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
    const auto size = KIO_TRY(co_await active_file_->AsyncWrite(tombstone));

    auto& active_stats = stats_.data_files[active_file_->FileId()];
    active_stats.total_bytes += size;

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
        KIO_TRY(co_await worker_.AsyncFdatasync(active_file_->Handle().Get()));
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
    const uint64_t actual_size = active_file_->Size();
    const int active_fd = active_file_->Handle().Get();

    KIO_TRY(co_await worker_.AsyncFsync(active_file_->Handle().Get()));
    KIO_TRY(co_await worker_.AsyncFtruncate(active_fd, static_cast<off_t>(actual_size)));
    KIO_TRY(co_await active_file_->AsyncClose());

    KIO_TRY(co_await CreateAndSetActiveFile());

    stats_.file_rotations_total++;
    co_return {};
}

Task<Result<void>> Partition::CreateAndSetActiveFile()
{
    uint64_t const new_id = file_id_gen_.Next();
    int const new_fd =
        KIO_TRY(co_await worker_.AsyncOpenat(GetDataFilePath(new_id), config_.write_flags, config_.file_mode));

    active_file_ = std::make_unique<DataFile>(new_fd, new_id, worker_, config_);
    co_await worker_.AsyncFallocate(new_fd, 0, static_cast<off_t>(config_.max_file_size));
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

DetachedTask Partition::CompactionLoop()
{
    co_await SwitchToWorker(worker_);
    ALOG_INFO("Partition {} compaction loop starting", partition_id_);

    const auto st = worker_.GetStopToken();
    while (!st.stop_requested() && !shutting_down_.load())
    {
        co_await compaction_trigger_.WaitFor(worker_, config_.compaction_interval_s);
        if (shutting_down_.load())
            break;

        if (auto res = co_await Compact(); !res.has_value())
        {
            ALOG_ERROR("Partition {}: co_await compaction failed", partition_id_);
        }
        compaction_trigger_.Reset();
    }
    ALOG_INFO("Partition {} compaction loop exiting", partition_id_);
}

// we get only files that match our data file format and silently skip all the rest
std::vector<uint64_t> Partition::ScanDataFiles() const
{
    const std::filesystem::path dir_to_scan = DbPath();
    if (!std::filesystem::exists(dir_to_scan) || !std::filesystem::is_directory(DbPath()))
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
        std::string file_name = dir_entry.path().filename().string();
        std::string_view sv(file_name);

        if (sv.size() <= (prefix_len + ext_len) || !sv.starts_with(kDataFilePrefix) ||
            !sv.ends_with(kDataFileExtension))
        {
            continue;
        }

        std::string_view file_id_str = sv.substr(prefix_len, sv.size() - prefix_len - ext_len);
        uint64_t file_id{};
        if (auto [p, ec] = std::from_chars(file_id_str.data(), file_id_str.data() + file_id_str.size(), file_id);
            ec == std::errc{} && p == file_id_str.data() + file_id_str.size())
        {
            result.push_back(file_id);
        }
    }
    std::ranges::sort(result);
    return result;
}

Task<Result<void>> Partition::RecoverFromHintFile(const FileHandle& fh, const uint64_t file_id)
{
    const int fd = fh.Get();
    const auto buf = KIO_TRY(co_await ReadFileContent(worker_, fd));
    if (buf.empty())
    {
        co_return {};
    }

    std::span buf_span(buf);
    while (!buf_span.empty())
    {
        const auto [entry, size] = KIO_TRY(HintEntry::Deserialize(buf_span));
        std::string key(entry.key);
        keydir_[key] = ValueLocation{
            .file_id = file_id, .offset = entry.offset, .total_size = entry.size, .timestamp_ns = entry.timestamp_ns};
        buf_span = buf_span.subspan(size);
    }
    co_return {};
}

Task<Result<void>> Partition::RecoverFromDataFile(const FileHandle& fh, uint64_t file_id)
{
    const int fd = fh.Get();
    auto file_size = KIO_TRY(GetFileSize(fd));
    if (file_size == 0)
    {
        co_return {};
    }

    ALOG_INFO("Recovering from data file {}, size: {} bytes", file_id, file_size);

    BytesMut buffer(config_.read_buffer_size * 2);
    uint64_t file_offset = 0;
    uint64_t entries_recovered = 0;
    uint64_t entries_skipped = 0;

    while (file_offset < file_size)
    {
        buffer.Reserve(config_.read_buffer_size);
        auto writable = buffer.WritableSpan();
        const uint64_t bytes_to_read = std::min(writable.size(), file_size - file_offset);

        const auto bytes_read =
            KIO_TRY(co_await worker_.AsyncReadAt(fd, writable.subspan(0, bytes_to_read), file_offset));

        if (bytes_read == 0)
        {
            break;
        }

        buffer.CommitWrite(bytes_read);
        file_offset += static_cast<uint64_t>(bytes_read);

        auto entries_result = RecoverDataFromBuffer(buffer, file_id, file_offset);

        if (!entries_result.has_value())
        {
            if (entries_result.error().value == kIoNeedMoreData)
            {
                continue;
            }
            ALOG_WARN("File {} recovery stopped: {}", file_id, entries_result.error());
            break;
        }

        entries_recovered += entries_result.value().first;
        entries_skipped += entries_result.value().second;

        if (buffer.ShouldCompact())
        {
            buffer.Compact();
        }
    }
    ALOG_INFO("Recovered {} entries from file {}", entries_recovered, file_id);
    co_return {};
}

Result<std::pair<uint64_t, uint64_t>> Partition::RecoverDataFromBuffer(BytesMut& buffer, const uint64_t file_id,
                                                                       const uint64_t file_offset)
{
    uint64_t entries_recovered = 0;
    uint64_t entries_skipped = 0;

    while (buffer.Remaining() >= kEntryFixedHeaderSize)
    {
        const auto readable = buffer.ReadableSpan();
        auto entry_result = DataEntry::Deserialize(readable);

        if (!entry_result.has_value())
            return std::unexpected(entry_result.error());

        auto entry = std::move(entry_result.value());
        const uint64_t bytes_consumed = buffer.Consumed();
        const uint64_t buffer_start = file_offset - buffer.Remaining() - bytes_consumed;
        const uint64_t entry_offset = buffer_start + bytes_consumed;

        buffer.Advance(entry.Size());

        if (entry.IsTombstone())
        {
            keydir_.erase(entry.GetKeyView());
            entries_skipped++;
            continue;
        }

        entries_recovered++;
        keydir_.insert_or_assign(entry.GetKeyView(), ValueLocation{.file_id = file_id,
                                                                   .offset = entry_offset,
                                                                   .total_size = entry.Size(),
                                                                   .timestamp_ns = entry.GetTimestamp()});
    }
    return std::make_pair(entries_recovered, entries_skipped);
}

Task<Result<void>> Partition::Recover()
{
    const auto files = ScanDataFiles();
    for (auto file_id : files)
    {
        stats_.data_files[file_id].total_bytes = std::filesystem::file_size(GetDataFilePath(file_id));
    }

    uint64_t max_file_id = 0;
    for (const auto file_id : files)
    {
        max_file_id = std::max(file_id, max_file_id);

        if (const auto hint_result = co_await TryRecoverFromHint(file_id); hint_result)
        {
            continue;
        }

        const auto fd =
            KIO_TRY(co_await worker_.AsyncOpenat(GetDataFilePath(file_id), config_.read_flags, config_.file_mode));
        FileHandle const data(fd);
        if (auto res = co_await RecoverFromDataFile(data, file_id); !res.has_value())
        {
            ALOG_ERROR("Failed to recover data file {}", file_id);
        }
    }

    for (const auto& loc : keydir_ | std::views::values)
    {
        auto& fs = stats_.data_files[loc.file_id];
        fs.live_bytes += loc.total_size;
        fs.live_entries++;
    }

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

    const auto fd = co_await worker_.AsyncOpenat(hint_path, config_.read_flags, config_.file_mode);
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
    uint64_t actual_file_id = ActiveFileId();

    if (active_fd > 0)
    {
        KIO_TRY(co_await worker_.AsyncFsync(active_fd));

        KIO_TRY(co_await worker_.AsyncFtruncate(active_fd, static_cast<off_t>(actual_size)));

        KIO_TRY(co_await active_file_->AsyncClose());
    }
    else
    {
        // close and remove as this active file was never used
        ALOG_DEBUG("The active file is empty, we will remove it, file_id {}", actual_file_id);
        KIO_TRY(co_await active_file_->AsyncClose());
        const auto path = config_.directory / std::format("partition_{}/data_{}.db", partition_id_, actual_file_id);
        KIO_TRY(co_await worker_.AsyncUnlinkAt(active_fd, path, config_.file_mode));
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
    }
    else
    {
        ALOG_INFO("Partition {}: compaction succeeded", partition_id_);
        stats_.compactions_total++;

        // Remove compacted files from FD cache
        for (const uint64_t src_id : ctx.src_file_ids)
        {
            fd_cache_.Remove(src_id);
        }
    }

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
}
