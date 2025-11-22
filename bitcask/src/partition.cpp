//
// Created by Yao ACHI on 21/11/2025.
//

#include "bitcask/include/partition.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

Partition::Partition(const BitcaskConfig& config, Worker& worker, const size_t partition_id) : worker_(worker), compactor_(worker, config, keydir_), config_(config), partition_id_(partition_id) {}

Task<Result<void>> Partition::put(std::string&& key, std::vector<char>&& value)
{
    // switch to the worker thread, there will be no switch if we are already on it
    co_await SwitchToWorker(worker_);

    // should we rotate?
    if (active_file_->should_rotate(config_.max_file_size))
    {
        KIO_TRY(co_await rotate_active_file());
    }

    DataEntry entry{std::move(key), std::move(value)};

    // append
    auto [offset, len] = KIO_TRY(co_await active_file_->async_write(entry));

    const ValueLocation new_loc{active_file_->file_id(), offset, len, entry.timestamp_ns};

    // check if it'd an overwriting
    if (const auto it = keydir_.find(entry.get_key()); it != keydir_.end())
    {
        auto& old_stats = stats_.data_files[it->second.file_id];
        old_stats.live_bytes -= it->second.total_size;
        old_stats.dead_entries--;
        old_stats.live_entries++;

        it->second = new_loc;
    }
    else
    {
        keydir_[entry.get_key()] = new_loc;
    }

    // new stats
    auto& new_stats = stats_.data_files[new_loc.file_id];
    new_stats.live_bytes += new_loc.total_size;
    new_stats.live_entries++;
    new_stats.total_bytes += new_loc.total_size;
    stats_.puts_total++;

    // it's easy to mistake Task<Result<void>> as Task<void>
    co_return {};
}

Task<Result<std::optional<std::vector<char>>>> Partition::get(const std::string& key)
{
    // switch to the worker thread, there will be no switch if we are already on it
    co_await SwitchToWorker(worker_);
    stats_.gets_total++;

    // lookup into the keydir first
    const auto it = keydir_.find(key);
    if (it == keydir_.end())
    {
        stats_.gets_miss_total++;
        co_return std::nullopt;
    }

    const auto& loc = it->second;
    const int fd = KIO_TRY(find_fd(it));

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

    // remove from keydir first as it is the source of truth
    auto& old_stats = stats_.data_files[it->second.file_id];
    old_stats.live_bytes -= it->second.total_size;
    old_stats.live_entries--;
    old_stats.dead_entries++;
    keydir_.erase(it);

    // Write tombstone
    const DataEntry tombstone(std::string(key), std::vector<char>{}, kFlagTombstone);
    auto [_, size] = KIO_TRY(co_await active_file_->async_write(tombstone));
    old_stats.total_bytes += size;

    // TODO check if the file need compaction
    if (old_stats.fragmentation() > config_.fragmentation_threshold)
    {
        // TODO signal compaction here
    }

    stats_.deletes_total++;

    co_return {};
}


Result<int> Partition::find_fd(SimpleKeydir::const_iterator it)
{
    if (it->second.file_id == active_file_->file_id())
    {
        return active_file_->handle().get();
    }
    const auto sealed_it = sealed_files_.find(it->second.file_id);
    if (sealed_it == sealed_files_.end())
    {
        // this should not happen normally, it's clearly a bug
        return std::unexpected(Error{ErrorCategory::Application, kAppInvalidArg});
    }
    return sealed_it->second.get();
}


Task<Result<DataEntry>> Partition::async_read_entry(const int fd, const uint64_t offset, const uint32_t size) const
{
    if (offset + size > size)
    {
        co_return std::unexpected(Error{ErrorCategory::Application, kAppInvalidArg});
    }
    std::vector<char> buffer(size);
    KIO_TRY(co_await worker_.async_read_exact_at(fd, buffer, offset));

    auto deserialize_result = DataEntry::deserialize(buffer);
    if (!deserialize_result.has_value())
    {
        ALOG_ERROR("Deserialization failed: {}", deserialize_result.error());
        co_return std::unexpected(Error{ErrorCategory::Application, kIoDeserialization});
    }

    co_return deserialize_result.value().first;
}

Task<Result<void>> Partition::rotate_active_file()
{
    uint64_t sealed_id = active_file_->file_id();

    // flush and close the active file
    KIO_TRY(co_await worker_.async_fsync(active_file_->handle().get()));
    KIO_TRY(co_await active_file_->async_close());

    // reopen as a readonly file
    const auto sealed_path = config_.directory / std::format("data_{}.db", sealed_id);
    int sealed_fd = KIO_TRY(co_await worker_.async_openat(sealed_path, config_.read_flags, config_.file_mode));
    sealed_files_.emplace(sealed_fd, FileHandle{sealed_fd});

    // create a new file id
    uint64_t new_id = generate_file_id();
    int new_fd = KIO_TRY(co_await worker_.async_openat(config_.directory / std::format("data_{}.db", new_id), config_.write_flags, config_.file_mode));
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
