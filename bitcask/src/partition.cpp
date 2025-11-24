#include "bitcask/include/partition.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

Partition::Partition(const BitcaskConfig& config, Worker& worker, const size_t partition_id) :
    worker_(worker), fd_cache_(worker, config.max_open_sealed_files), config_(config), partition_id_(partition_id), compaction_trigger_(worker), file_id_gen_(partition_id)
{
    // TODO: Recovery, initial file creation
}

Task<Result<void>> Partition::put(std::string&& key, std::vector<char>&& value)
{
    co_await SwitchToWorker(worker_);

    if (active_file_->should_rotate(config_.max_file_size))
    {
        KIO_TRY(co_await rotate_active_file());
    }

    DataEntry entry{std::move(key), std::move(value)};
    auto [offset, len] = KIO_TRY(co_await active_file_->async_write(entry));

    const ValueLocation new_loc{active_file_->file_id(), offset, len, entry.timestamp_ns};

    auto& dst_stats = stats_.data_files[new_loc.file_id];
    if (const auto it = keydir_.find(entry.get_key()); it != keydir_.end())
    {
        // Overwrite: decrement old file stats
        auto& old_stats = stats_.data_files[it->second.file_id];
        old_stats.live_bytes -= it->second.total_size;
        old_stats.live_entries--;
        old_stats.dead_entries++;

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
    co_await SwitchToWorker(worker_);

    const auto it = keydir_.find(key);
    if (it == keydir_.end())
    {
        co_return {};
    }

    // Update stats for old location
    auto& old_stats = stats_.data_files[it->second.file_id];
    old_stats.live_bytes -= it->second.total_size;
    old_stats.live_entries--;
    old_stats.dead_entries++;

    const uint64_t old_file_id = it->second.file_id;

    // Remove from KeyDir first as its our source of truth
    keydir_.erase(it);

    // Write tombstone
    const DataEntry tombstone(std::string(key), std::vector<char>{}, kFlagTombstone);
    auto [_, size] = KIO_TRY(co_await active_file_->async_write(tombstone));

    // Update active file stats (tombstone adds to total_bytes)
    auto& active_stats = stats_.data_files[active_file_->file_id()];
    active_stats.total_bytes += size;

    stats_.deletes_total++;

    // Check if a file needs compaction
    if (should_compact_file(old_file_id))
    {
        signal_compaction(old_file_id);
    }

    co_return {};
}


Task<Result<int>> Partition::find_fd(const uint64_t file_id)
{
    if (file_id == active_file_->file_id())
    {
        co_return active_file_->handle().get();
    }

    // FD is automatically opens if not cached
    const auto path = get_data_file_path(file_id);
    co_return co_await fd_cache_.get_or_open(file_id, path);
}

Task<Result<DataEntry>> Partition::async_read_entry(const int fd, const uint64_t offset, const uint32_t size) const
{
    std::vector<char> buffer(size);
    KIO_TRY(co_await worker_.async_read_exact_at(fd, buffer, offset));

    auto [entry, _] = KIO_TRY(DataEntry::deserialize(buffer));

    co_return entry;
}

Task<Result<void>> Partition::rotate_active_file()
{
    // Flush and close the active file
    KIO_TRY(co_await worker_.async_fsync(active_file_->handle().get()));
    KIO_TRY(co_await active_file_->async_close());

    // Add to FD cache (it will open on-demand when needed for reads)
    // No need to explicitly open here

    // Create a new active file
    uint64_t new_id = file_id_gen_.next();
    int new_fd = KIO_TRY(co_await worker_.async_openat(get_data_file_path(new_id), config_.write_flags, config_.file_mode));

    active_file_ = std::make_unique<DataFile>(new_fd, new_id, worker_, config_);

    // Pre-allocate
    co_await worker_.async_fallocate(new_fd, 0, static_cast<off_t>(config_.max_file_size));

    stats_.file_rotations_total++;

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

std::filesystem::path Partition::get_data_file_path(uint64_t file_id) const { return config_.directory / std::format("partition_{}/data_{}.db", partition_id_, file_id); }

DetachedTask Partition::compaction_loop()
{
    co_await SwitchToWorker(worker_);

    ALOG_INFO("Partition {} compaction loop starting", partition_id_);

    const auto st = worker_.get_stop_token();
    while (!st.stop_requested())
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

        // Find files to compact
        auto fragmented_files = find_fragmented_files();

        if (fragmented_files.empty())
        {
            ALOG_DEBUG("Partition {}: no files to compact", partition_id_);
            compaction_trigger_.reset();
            continue;
        }

        ALOG_INFO("Partition {}: compacting {} files", partition_id_, fragmented_files.size());

        stats_.compaction_running = true;

        // Generate destination file ID
        const uint64_t dst_file_id = file_id_gen_.next();

        // Create compaction context
        compactor::CompactionContext ctx(worker_, config_, keydir_, stats_, partition_id_, dst_file_id, std::move(fragmented_files), compaction_limits_);

        auto result = co_await compactor::compact_files(ctx);

        if (!result.has_value())
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

            // The new compacted file will be added to cache on first read
        }

        // Reset trigger
        compaction_trigger_.reset();
        stats_.compaction_running = false;
    }

    ALOG_INFO("Partition {} compaction loop exiting", partition_id_);
}
