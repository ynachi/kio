//
// Created by Yao ACHI on 08/11/2025.
//

#include "bitcask/include/keydir.h"

#include <memory>
#include <stdexcept>

namespace bitcask
{
    KeyDir::KeyDir(const size_t shard_count) : shard_count_(shard_count)
    {
        if (shard_count == 0)
        {
            throw std::invalid_argument("shard_count cannot be zero");
        }

        // n and n - 1 are complements in bit repr when power of two
        if ((shard_count & shard_count - 1) != 0)
        {
            throw std::invalid_argument("shard_count must be a power of two");
        }

        shards_.reserve(shard_count_);

        for (size_t i = 0; i < shard_count_; ++i)
        {
            shards_.push_back(std::make_unique<Shard>());
        }
    }

    std::unordered_map<std::string, ValueLocation> KeyDir::snapshot() const
    {
        std::unordered_map<std::string, ValueLocation> full_snapshot;

        for (const auto& shard_ptr: shards_)
        {
            std::shared_lock lock(shard_ptr->mu);
            full_snapshot.insert(shard_ptr->index_.begin(), shard_ptr->index_.end());
        }

        return full_snapshot;
    }

    [[nodiscard]]
    KeyDir::Shard& KeyDir::shard(std::string_view key) const
    {
        const auto hash = std::hash<std::string_view>{}(key);
        const auto shard_id = hash & (shard_count_ - 1);
        return *shards_.at(shard_id);
    }

    void KeyDir::Shard::put(std::string&& key, ValueLocation loc)
    {
        std::lock_guard lock(mu);
        index_.insert_or_assign(std::move(key), loc);
    }

    std::optional<ValueLocation> KeyDir::Shard::get(const std::string& key)
    {
        std::shared_lock lock(mu);
        const auto it = index_.find(key);
        if (it == index_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    void KeyDir::Shard::put_if_newer(std::string&& key, ValueLocation loc)
    {
        std::lock_guard lock(mu);

        // 1. Key doesn't exist? Add it.
        if (const auto it = index_.find(key); it == index_.end())
        {
            index_.emplace(std::move(key), loc);
        }
        // 2. Key exists? Only replace it if the new timestamp is newer.
        else if (loc.timestamp > it->second.timestamp)
        {
            it->second = loc;
        }
        // 3. Otherwise, do nothing (the on-disk entry is old).
    }

}  // namespace bitcask
