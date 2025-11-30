//
// Created by Yao ACHI on 23/11/2025.
//

#include "bitcask/include/fd_cache.h"

namespace bitcask
{
    kio::Task<kio::Result<int>> FDCache::get_or_open(uint64_t file_id, const std::filesystem::path& path)
    {
        // Cache hit
        if (const auto it = cache_.find(file_id); it != cache_.end())
        {
            stats_.hits++;
            touch(file_id);
            co_return it->second.handle.get();
        }

        // Cache miss
        stats_.misses++;

        // Evict if at capacity
        if (cache_.size() >= max_open_files_)
        {
            evict_oldest();
        }

        // Open file
        int fd = KIO_TRY(co_await worker_.async_openat(path, O_RDONLY, 0));

        // Add to cache
        lru_list_.push_front(file_id);
        cache_[file_id] = CacheEntry{FileHandle(fd), path, lru_list_.begin()};

        ALOG_DEBUG("FdCache: Opened file {} (cache size: {})", file_id, cache_.size());

        co_return fd;
    }

    void FDCache::remove(uint64_t file_id)
    {
        if (const auto it = cache_.find(file_id); it != cache_.end())
        {
            lru_list_.erase(it->second.lru_iter);
            cache_.erase(it);
            ALOG_DEBUG("FdCache: Removed file {} (cache size: {})", file_id, cache_.size());
        }
    }

    void FDCache::touch(const uint64_t file_id)
    {
        const auto it = cache_.find(file_id);
        if (it == cache_.end()) return;

        auto& entry = it->second;
        lru_list_.erase(entry.lru_iter);
        lru_list_.push_front(file_id);
        entry.lru_iter = lru_list_.begin();
    }

    void FDCache::evict_oldest()
    {
        if (lru_list_.empty()) return;

        uint64_t oldest_id = lru_list_.back();
        lru_list_.pop_back();
        cache_.erase(oldest_id);
        stats_.evictions++;

        ALOG_DEBUG("FdCache: Evicted file {} (cache size: {})", oldest_id, cache_.size());
    }

}  // namespace bitcask
