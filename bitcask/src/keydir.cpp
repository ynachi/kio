//
// Created by Yao ACHI on 08/11/2025.
//

#include "bitcask/include/keydir.h"

#include <memory>
#include <mutex>
#include <stdexcept>

namespace bitcask
{
    KeyDir::KeyDir(const size_t shard_count) : shard_count_(shard_count)
    {
        if (shard_count == 0)
        {
            throw std::invalid_argument("shard_count cannot be zero");
        }

        if (!std::has_single_bit(shard_count))
        {
            throw std::invalid_argument("shard_count must be a power of two");
        }

        shards_.reserve(shard_count_);

        for (size_t i = 0; i < shard_count_; ++i)
        {
            shards_.push_back(std::make_unique<Shard>());
        }
    }

    KeyDir::Shard& KeyDir::shard_mut(std::string_view key) const
    {
        const auto hash = std::hash<std::string_view>{}(key);
        const auto shard_id = hash & (shard_count_ - 1);
        return *shards_.at(shard_id);
    }

    void KeyDir::Shard::put(std::string&& key, const ValueLocation& loc)
    {
        std::lock_guard lock(mu);
        if (const auto it = index_.find(key); it != index_.end())
        {
            auto& old_stats = per_files_stats[it->second.file_id];
            old_stats.live_bytes -= it->second.total_size;

            it->second = loc;
        }

        per_files_stats[loc.file_id].live_bytes += loc.total_size;
        index_[std::move(key)] = loc;
    }

    std::optional<ValueLocation> KeyDir::Shard::get(const std::string& key) const
    {
        std::shared_lock lock(mu);
        const auto it = index_.find(key);
        if (it == index_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<std::pair<uint64_t, uint64_t>> KeyDir::Shard::remove(const std::string& key)
    {
        std::lock_guard lock(mu);
        if (const auto it = index_.find(key); it != index_.end())
        {
            auto& stats = per_files_stats[it->second.file_id];
            stats.live_bytes -= it->second.total_size;
            stats.tombstones_count++;
            auto file_id = it->second.file_id;
            index_.erase(it);
            return std::pair{file_id, stats.live_bytes};
        }
        return std::nullopt;
    }

    void KeyDir::Shard::put_if_newer(std::string&& key, const ValueLocation& loc)
    {
        std::lock_guard lock(mu);

        if (const auto it = index_.find(key); it == index_.end())
        {
            index_.try_emplace(std::move(key), loc);
            per_files_stats[loc.file_id].live_bytes += loc.total_size;
        }
        else if (loc.timestamp_ns > it->second.timestamp_ns)
        {
            const auto old = it->second;  // remember old value
            it->second = loc;

            per_files_stats[old.file_id].live_bytes -= old.total_size;
            per_files_stats[loc.file_id].live_bytes += loc.total_size;
        }
        // else: do nothing (old on-disk entry is newer)
    }

    bool KeyDir::Shard::update_if_matches(const std::string& key, const ValueLocation& new_loc, uint64_t expected_file_id, uint64_t expected_offset)
    {
        std::unique_lock lock(mu);
        const auto it = index_.find(key);

        // If key doesn't exist, or points to something else, fail.
        if (it == index_.end())
        {
            return false;
        }

        if (it->second.file_id != expected_file_id || it->second.offset != expected_offset)
        {
            return false;
        }

        // Match confirmed. Perform update and stats maintenance.

        // 1. Decrement old stats
        auto& old_stats = per_files_stats[it->second.file_id];
        old_stats.live_bytes -= it->second.total_size;
        old_stats.tombstones_count++;

        // 2. Increment new stats
        auto& new_stats = per_files_stats[new_loc.file_id];
        new_stats.live_bytes += new_loc.total_size;
        new_stats.live_records_count++;

        // 3. Update index
        it->second = new_loc;
        return true;
    }

    SimpleKeydir KeyDir::snapshot() const
    {
        size_t total_keys = 0;
        for (const auto& shard: shards_)
        {
            std::shared_lock lock(shard->mu);
            total_keys += shard->index_.size();
        }

        SimpleKeydir full_snapshot;
        // avoid repeated rehash
        full_snapshot.reserve(total_keys);

        for (const auto& shard_ptr: shards_)
        {
            std::shared_lock lock(shard_ptr->mu);
            full_snapshot.insert(shard_ptr->index_.begin(), shard_ptr->index_.end());
        }

        return full_snapshot;
    }

    [[nodiscard]]
    const KeyDir::Shard& KeyDir::shard(std::string_view key) const
    {
        const auto hash = std::hash<std::string_view>{}(key);
        const auto shard_id = hash & (shard_count_ - 1);
        return *shards_.at(shard_id);
    }

    KeyDir::Stats KeyDir::snapshot_stats() const
    {
        Stats s;

        for (const auto& shard: shards_)
        {
            std::shared_lock lock(shard->mu);
            // shared/read lock
            for (const auto& [file_id, fs]: shard->per_files_stats)
            {
                auto& total = s.data_files[file_id];
                total.live_bytes += fs.live_bytes;
                total.live_records_count += shard->index_.size();
                total.tombstones_count += fs.tombstones_count;
            }
        }

        s.data_files_count = s.data_files.size();
        return s;
    }


}  // namespace bitcask
