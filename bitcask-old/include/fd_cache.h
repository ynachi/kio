//
// Created by Yao ACHI on 23/11/2025.
//

#ifndef KIO_FD_CACHE_H
#define KIO_FD_CACHE_H
#include "file_handle.h"
#include "kio/core/worker.h"

#include <list>

namespace bitcask
{
/**
 * @brief LRU cache for file descriptors
 *
 * Prevents FD exhaustion by:
 * 1. Limiting max open files (e.g., 100)
 * 2. Automatically evicting least recently used
 * 3. Reopening files on-demand
 */
class FDCache
{
public:
    explicit FDCache(kio::io::Worker& worker, size_t max_files = 100) : worker_(worker), max_open_files_(max_files) {}

    /**
     * @brief Get FD for file_id, opening if not cached
     */
    kio::Task<kio::Result<int>> GetOrOpen(uint64_t file_id, const std::filesystem::path& path);
    /**
     * @brief Remove file from cache (e.g., after compaction)
     */
    void Remove(uint64_t file_id);
    /**
     * @brief Clear all cached FDs
     */
    void Clear()
    {
        cache_.clear();
        lru_list_.clear();
    }
    // Stats
    struct Stats
    {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;

        [[nodiscard]] double HitRate() const
        {
            return hits + misses > 0 ? static_cast<double>(hits) / static_cast<double>(hits + misses) : 0.0;
        }
    };

    [[nodiscard]] const Stats& GetStats() const { return stats_; }
    [[nodiscard]] size_t Size() const { return cache_.size(); }

private:
    struct CacheEntry
    {
        FileHandle handle;
        std::filesystem::path path;
        std::list<uint64_t>::iterator lru_iter;
    };

    kio::io::Worker& worker_;
    size_t max_open_files_;
    std::unordered_map<uint64_t, CacheEntry> cache_;
    std::list<uint64_t> lru_list_;
    Stats stats_;

    void Touch(uint64_t file_id);
    void EvictOldest();
};
}  // namespace bitcask

#endif  // KIO_FD_CACHE_H
