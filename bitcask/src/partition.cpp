#include "bitcask/include/partition.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

Partition::Partition(const BitcaskConfig& config, Worker& worker, const size_t partition_id) :
    keydir_(&mem_pool_), worker_(worker), fd_cache_(worker, config.max_open_sealed_files), file_id_gen_(partition_id),
    config_(config), partition_id_(partition_id), compaction_trigger_(worker)
{
    //  Recovery, compaction loop delegated to open factory method
}

Task<Result<void>> Partition::put(std::string&& key, std::vector<char>&& value)
{
    // Assert partition is initialized
    assert(active_file_ && "Partition not initialized - active_file_ is null");

    co_await SwitchToWorker(worker_);

    if (active_file_->should_rotate(config_.max_file_size))
    {
        KIO_TRY(co_await rotate_active_file());
    }

    DataEntry entry{std::move(key), std::move(value)};
    auto [offset, len] = KIO_TRY(co_await active_file_->async_write(entry));
    ALOG_TRACE("Wrote entry to file {} at offset {} with size {}", active_file_->file_id(), offset, len);

    const ValueLocation new_loc{active_file_->file_id(), offset, len, entry.timestamp_ns};

    auto& dst_stats = stats_.data_files[new_loc.file_id];
    if (const auto it = keydir_.find(entry.get_key()); it != keydir_.end())
    {
        // Overwrite: decrement old file stats
        auto& old_stats = stats_.data_files[it->second.file_id];
        old_stats.live_bytes -= it->second.total_size;
        old_stats.live_entries--;

        it->second = new_loc;
    }
    else
    {
        // New key
        keydir_[entry.get_key()] = new_loc;
    }

    // Increment new file stats
    dst_stats.live_bytes += new_loc.total_size;
    dst_stats.live_entries++;
    dst_stats.total_bytes += new_loc.total_size;

    stats_.puts_total++;

    co_return {};
}

Task<Result<std::optional<std::vector<char>>>> Partition::get(const std::string& key)
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
    const int fd = KIO_TRY(co_await find_fd(loc.file_id));

    ALOG_TRACE("Reading entry from file {} at offset {} with size {}", loc.file_id, loc.offset, loc.total_size);
    DataEntry entry = KIO_TRY(co_await async_read_entry(fd, loc.offset, loc.total_size));

    if (entry.is_tombstone())
    {
        stats_.gets_miss_total++;
        co_return std::nullopt;
    }

    co_return entry.value;
}

Task<Result<void>> Partition::del(const std::string& key)
{
    // Assert partition is initialized
    assert(active_file_ && "Partition not initialized - active_file_ is null");

    co_await SwitchToWorker(worker_);

    stats_.deletes_total++;

    const auto it = keydir_.find(key);
    if (it == keydir_.end())
    {
        co_return {};
    }

    // Update stats for old location
    auto& old_stats = stats_.data_files[it->second.file_id];
    old_stats.live_bytes -= it->second.total_size;
    old_stats.live_entries--;

    const uint64_t old_file_id = it->second.file_id;

    // Remove from KeyDir first as it's our source of truth
    keydir_.erase(it);

    // Write tombstone
    const DataEntry tombstone(std::string(key), std::vector<char>{}, kFlagTombstone);
    auto [_, size] = KIO_TRY(co_await active_file_->async_write(tombstone));

    // Update active file stats (tombstone adds to total_bytes)
    auto& active_stats = stats_.data_files[active_file_->file_id()];
    active_stats.total_bytes += size;

    // Check if a file needs compaction
    if (should_compact_file(old_file_id))
    {
        signal_compaction(old_file_id);
    }

    co_return {};
}

Task<Result<int>> Partition::find_fd(const uint64_t file_id)
{
    // if file id is the active one, flush first
    if (file_id == active_file_->file_id())
    {
        KIO_TRY(co_await worker_.AsyncFdatasync(active_file_->handle().get()));
    }

    const auto path = get_data_file_path(file_id);
    co_return co_await fd_cache_.get_or_open(file_id, path);
}

Task<Result<DataEntry>> Partition::async_read_entry(const int fd, const uint64_t offset, const uint32_t size) const
{
    ALOG_TRACE("Reading entry from file {} at offset {} with size {}", fd, offset, size);
    std::vector<char> buffer(size);
    KIO_TRY(co_await worker_.AsyncReadExactAt(fd, buffer, offset));
    ALOG_TRACE("Read entry from file {} at offset {} with size {}", fd, offset, size);

    auto [entry, _] = KIO_TRY(DataEntry::deserialize(buffer));
    co_return entry;
}

Task<Result<void>> Partition::rotate_active_file()
{
    // Assert partition is initialized
    assert(active_file_ && "Partition not initialized - active_file_ is null");

    // Get the actual written size before flushing and closing
    const uint64_t actual_size = active_file_->size();
    const int active_fd = active_file_->handle().get();

    // Flush
    KIO_TRY(co_await worker_.AsyncFsync(active_file_->handle().get()));

    // truncate to the actual used space
    KIO_TRY(co_await worker_.AsyncFtruncate(active_fd, static_cast<off_t>(actual_size)));

    // close
    KIO_TRY(co_await active_file_->async_close());

    // No need to add to FD cache as it will be done when needed

    // Create a new active file
    KIO_TRY(co_await create_and_set_active_file());

    stats_.file_rotations_total++;

    co_return {};
}

Task<Result<void>> Partition::create_and_set_active_file()
{
    uint64_t new_id = file_id_gen_.next();
    int new_fd =
            KIO_TRY(co_await worker_.AsyncOpenat(get_data_file_path(new_id), config_.write_flags, config_.file_mode));

    active_file_ = std::make_unique<DataFile>(new_fd, new_id, worker_, config_);

    // Pre-allocate
    // TODO: remove to fix test for now
    co_await worker_.AsyncFallocate(new_fd, 0, static_cast<off_t>(config_.max_file_size));

    co_return {};
}

bool Partition::should_compact_file(const uint64_t file_id) const
{
    const auto it = stats_.data_files.find(file_id);
    if (it == stats_.data_files.end()) return false;

    return it->second.fragmentation() >= config_.fragmentation_threshold;
}

void Partition::signal_compaction(const uint64_t file_id)
{
    if (should_compact_file(file_id))
    {
        compaction_trigger_.notify();
    }
}

std::vector<uint64_t> Partition::find_fragmented_files() const
{
    std::vector<uint64_t> result;

    for (const auto& [file_id, file_stats]: stats_.data_files)
    {
        // Never compact the active file
        if (file_id == active_file_->file_id()) continue;

        if (file_stats.fragmentation() >= config_.fragmentation_threshold)
        {
            result.push_back(file_id);
        }
    }

    return result;
}

std::filesystem::path Partition::get_data_file_path(uint64_t file_id) const
{
    return config_.directory / std::format("partition_{}/data_{}.db", partition_id_, file_id);
}
std::filesystem::path Partition::get_hint_file_path(uint64_t file_id) const
{
    return config_.directory / std::format("partition_{}/hint_{}.ht", partition_id_, file_id);
}

DetachedTask Partition::compaction_loop()
{
    co_await SwitchToWorker(worker_);

    ALOG_INFO("Partition {} compaction loop starting", partition_id_);

    const auto st = worker_.GetStopToken();
    while (!st.stop_requested() && !shutting_down_.load())
    {
        // Wait for event or timer
        if (co_await compaction_trigger_.wait_for(config_.compaction_interval_s))
        {
            ALOG_INFO("Partition {}: compaction triggered by event", partition_id_);
        }
        else
        {
            ALOG_INFO("Partition {}: compaction triggered by timer", partition_id_);
        }

        // Check shutdown again after waking up
        if (shutting_down_.load())
        {
            break;
        }

        // compact all fragmented files
        if (auto res = co_await compact(); !res.has_value())
        {
            ALOG_ERROR("Partition {}: co_await compaction failed", partition_id_);
        }
        // The new compacted file will be added to cache on first read

        // Reset trigger
        compaction_trigger_.reset();
        stats_.compaction_running = false;
    }

    ALOG_INFO("Partition {} compaction loop exiting", partition_id_);
}

// we get only files that match our data file format and silently skip all the rest
std::vector<uint64_t> Partition::scan_data_files() const
{
    const std::filesystem::path dir_to_scan = db_path();
    if (!std::filesystem::exists(dir_to_scan) || !std::filesystem::is_directory(db_path()))
    {
        ALOG_ERROR("Data directory {} does not exist or is not a directory", dir_to_scan.string());
        return {};
    }

    ALOG_DEBUG("Scanning data directory {}", dir_to_scan.string());

    std::vector<uint64_t> result;
    constexpr size_t prefix_len = kDataFilePrefix.size();
    constexpr size_t ext_len = kDataFileExtension.size();

    for (const auto& dir_entry: std::filesystem::directory_iterator(dir_to_scan))
    {
        // skip early
        if (!dir_entry.is_regular_file()) continue;

        std::string file_name = dir_entry.path().filename().string();
        std::string_view sv(file_name);

        // skip early
        if (sv.size() <= (prefix_len + ext_len) || !sv.starts_with(kDataFilePrefix) ||
            !sv.ends_with(kDataFileExtension))
        {
            continue;
        }

        // extract ID
        std::string_view file_id_str = sv.substr(prefix_len, sv.size() - prefix_len - ext_len);

        // now parse
        uint64_t file_id;

        // verify success and push eventually
        if (auto [p, ec] = std::from_chars(file_id_str.data(), file_id_str.data() + file_id_str.size(), file_id);
            ec == std::errc{} && p == file_id_str.data() + file_id_str.size())
        {
            result.push_back(file_id);
        }
    }

    // sort the result
    std::ranges::sort(result);

    return result;
}

Task<Result<void>> Partition::recover_from_hint_file(const FileHandle& fh, const uint64_t file_id)
{
    const int fd = fh.get();

    // hint files are small, read entirely
    const auto buf = KIO_TRY(co_await read_file_content(worker_, fd));
    if (buf.empty())
    {
        co_return {};
    }

    std::span buf_span(buf);

    while (!buf_span.empty())
    {
        const auto res = struct_pack::deserialize<HintEntry>(buf_span);
        if (!res.has_value())
        {
            ALOG_ERROR("Failed to deserialize entry: {}", res.error().message());
            co_return std::unexpected(Error{ErrorCategory::kSerialization, kIoDeserialization});
        }

        const auto& entry = res.value();
        const size_t bytes_this_entry = struct_pack::get_needed_size(entry);

        keydir_[std::string(entry.key)] = ValueLocation{file_id, entry.entry_pos, entry.total_sz, entry.timestamp_ns};
        buf_span = buf_span.subspan(bytes_this_entry);
    }

    co_return {};
}

Task<Result<void>> Partition::recover_from_data_file(const FileHandle& fh, uint64_t file_id)
{
    const int fd = fh.get();
    auto file_size = KIO_TRY(get_file_size(fd));

    if (file_size == 0)
    {
        ALOG_DEBUG("File {} is empty, skipping recovery", file_id);
        co_return {};
    }

    ALOG_INFO("Recovering from data file {}, size: {} bytes", file_id, file_size);

    BytesMut buffer(config_.read_buffer_size * 2);
    uint64_t file_offset = 0;
    uint64_t entries_recovered = 0;
    uint64_t entries_skipped = 0;

    while (file_offset < file_size)
    {
        buffer.reserve(config_.read_buffer_size);
        auto writable = buffer.writable_span();

        const uint64_t bytes_to_read = std::min(writable.size(), file_size - file_offset);
        const auto bytes_read =
                KIO_TRY(co_await worker_.AsyncReadAt(fd, writable.subspan(0, bytes_to_read), file_offset));

        if (bytes_read == 0)
        {
            if (!buffer.is_empty())
            {
                // Warn about leftover bytes at EOF (likely corruption or incomplete write)
                ALOG_WARN("File {} has {} bytes of incomplete data at EOF", file_id, buffer.remaining());
            }
            break;
        }

        buffer.commit_write(bytes_read);
        file_offset += static_cast<uint64_t>(bytes_read);

        // Parse entries
        auto entries_result = recover_data_from_buffer(buffer, file_id, file_offset);

        if (!entries_result.has_value())
        {
            if (entries_result.error().value == kIoNeedMoreData)
            {
                continue;
            }

            // Log a warning and stop processing this file.
            // This allows valid entries recovered so far to remain in the KeyDir.
            ALOG_WARN("File {} recovery stopped due to junk/corruption at offset {}: {}", file_id, file_offset,
                      entries_result.error());
            break;
        }

        entries_recovered += entries_result.value().first;
        entries_skipped += entries_result.value().second;

        if (buffer.should_compact())
        {
            buffer.compact();
        }
        ALOG_DEBUG("Recovering stats for file={}: recovered={} skipped={}", file_id, entries_recovered,
                   entries_skipped);
    }

    ALOG_INFO("Recovered {} entries ({} tombstones skipped) from file {}", entries_recovered, entries_skipped, file_id);

    co_return {};
}

Result<std::pair<uint64_t, uint64_t>> Partition::recover_data_from_buffer(BytesMut& buffer, const uint64_t file_id,
                                                                          uint64_t file_offset)
{
    uint64_t entries_recovered = 0;
    uint64_t entries_skipped = 0;

    while (buffer.remaining() >= kEntryFixedHeaderSize)
    {
        const auto readable = buffer.readable_span();
        auto entry_result = DataEntry::deserialize(readable);

        if (!entry_result.has_value())
        {
            return std::unexpected(entry_result.error());
        }

        auto [entry, entry_size] = std::move(entry_result.value());

        // Calculate absolute offset in file
        const uint64_t bytes_consumed = buffer.consumed();
        const uint64_t buffer_start = file_offset - buffer.remaining() - bytes_consumed;
        const uint64_t entry_offset = buffer_start + bytes_consumed;

        buffer.advance(entry_size);

        if (entry.is_tombstone())
        {
            keydir_.erase(entry.key);
            entries_skipped++;
            continue;
        }

        entries_recovered++;
        keydir_[entry.key] =
                ValueLocation{file_id, entry_offset, static_cast<uint32_t>(entry_size), entry.timestamp_ns};
    }

    return std::make_pair(entries_recovered, entries_skipped);
}

Task<Result<void>> Partition::recover()
{
    const auto files = scan_data_files();

    // Get file sizes
    ALOG_INFO("Found {} data files", files.size());
    for (auto file_id: files)
    {
        stats_.data_files[file_id].total_bytes = std::filesystem::file_size(get_data_file_path(file_id));
    }

    // Recover each file
    for (const auto file_id: files)
    {
        // Try the hint file first
        if (const auto hint_result = co_await try_recover_from_hint(file_id); hint_result)
        {
            // Success, skip to the next file
            ALOG_DEBUG("Recovered from hint file {}", file_id);
            continue;
        }

        // Fallback to a data file
        const auto fd = KIO_TRY(
                co_await worker_.AsyncOpenat(get_data_file_path(file_id), config_.read_flags, config_.file_mode));
        FileHandle data(fd);
        if (auto res = co_await recover_from_data_file(data, file_id); !res.has_value())
        {
            // Do not exit on a single file failure, skip to the next
            // TODO: add a flag for the user to decide what to do in such cases
            ALOG_ERROR("Failed to recover from data file {}, skipping the rest of entries", file_id);
        }
        ALOG_DEBUG("Recovered from data file {}", file_id);
    }

    // Compute live stats
    for (const auto& loc: keydir_ | std::views::values)
    {
        auto& fs = stats_.data_files[loc.file_id];
        fs.live_bytes += loc.total_size;
        fs.live_entries++;
    }

    // create and set an active file
    KIO_TRY(co_await create_and_set_active_file());

    co_return {};
}

// Helper to try hint file recovery
Task<bool> Partition::try_recover_from_hint(uint64_t file_id)
{
    const auto hint_path = get_hint_file_path(file_id);
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
    const auto result = co_await recover_from_hint_file(hint, file_id);
    if (!result.has_value())
    {
        ALOG_ERROR("Failed to recover hint file {}", file_id);
    }
    co_return result.has_value();
}

Task<Result<std::unique_ptr<Partition>>> Partition::open(const BitcaskConfig& config, Worker& worker,
                                                         const size_t partition_id)
{
    ALOG_INFO("Opening partition {}", partition_id);
    std::unique_ptr<Partition> partition(new Partition(config, worker, partition_id));
    KIO_TRY(co_await partition->recover());
    if (config.auto_compact)
    {
        // run the compaction loop in the background
        partition->compaction_loop().detach();
    }
    // if durability not set, start periodic sync
    if (!config.sync_on_write)
    {
        partition->background_sync().detach();
    }
    co_return std::move(partition);
}

Task<Result<void>> Partition::seal_active_file()
{
    co_await SwitchToWorker(worker_);

    // Check if there is an active file to seal and if it has content
    if (active_file_ == nullptr)
    {
        co_return {};
    }

    const uint64_t actual_size = active_file_->size();
    const int active_fd = active_file_->handle().get();
    uint64_t actual_file_id = active_file_id();

    if (active_fd > 0)
    {
        KIO_TRY(co_await worker_.AsyncFsync(active_fd));

        KIO_TRY(co_await worker_.AsyncFtruncate(active_fd, static_cast<off_t>(actual_size)));

        KIO_TRY(co_await active_file_->async_close());
    }
    else
    {
        // close and remove as this active file was never used
        ALOG_DEBUG("The active file is empty, we will remove it, file_id {}", actual_file_id);
        KIO_TRY(co_await active_file_->async_close());
        const auto path = config_.directory / std::format("partition_{}/data_{}.db", partition_id_, actual_file_id);
        KIO_TRY(co_await worker_.AsyncUnlinkAt(active_fd, path, config_.file_mode));
    }

    // Remove pointer to prevent double-closing in dtor
    active_file_.reset();

    co_return {};
}

Task<Result<void>> Partition::async_close()
{
    ALOG_INFO("Partition {} closing...", partition_id_);

    // Signal shutdown to compaction loop
    shutting_down_.store(true);

    // Wake up the compaction loop so it can exit cleanly

    compaction_trigger_.notify();

    // Seal the active file if it exists and hasn't been sealed by rotation
    KIO_TRY(co_await seal_active_file());

    // The worker is not owned by the partition. It should not close it.

    ALOG_INFO("Partition {} closed successfully.", partition_id_);
    co_return {};
}

Task<Result<void>> Partition::compact()
{
    auto fragmented_files = find_fragmented_files();

    if (fragmented_files.empty())
    {
        ALOG_DEBUG("Partition {}: no files to compact", partition_id_);
        compaction_trigger_.reset();
        co_return {};
    }

    ALOG_INFO("Partition {}: compacting {} files", partition_id_, fragmented_files.size());

    stats_.compaction_running = true;

    // Generate destination file ID
    const uint64_t dst_file_id = file_id_gen_.next();

    // Create compaction context
    compactor::CompactionContext ctx(worker_, config_, keydir_, stats_, partition_id_, dst_file_id,
                                     std::move(fragmented_files), compaction_limits_);

    if (auto result = co_await compactor::compact_files(ctx); !result.has_value())
    {
        ALOG_ERROR("Partition {}: compaction failed: {}", partition_id_, result.error());
        stats_.compactions_failed++;
    }
    else
    {
        ALOG_INFO("Partition {}: compaction succeeded", partition_id_);
        stats_.compactions_total++;

        // Remove compacted files from FD cache
        for (const uint64_t src_id: ctx.src_file_ids)
        {
            fd_cache_.remove(src_id);
        }
    }

    co_return {};
}

Task<Result<void>> Partition::sync()
{
    co_await SwitchToWorker(worker_);
    if (active_file_)
    {
        KIO_TRY(co_await worker_.AsyncFsync(active_file_->handle().get()));
    }
    co_return {};
}

DetachedTask Partition::background_sync()
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

        if (auto res = co_await sync(); !res.has_value())
        {
            ALOG_ERROR("Partition {}: failed to sync: {}", partition_id_, res.error());
            // do not exit
        }
    }
    ALOG_INFO("Partition {} background sync loop exiting", partition_id_);
}
