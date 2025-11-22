//
// Created by Yao ACHI on 08/11/2025.
//

#ifndef KIO_KEYDIR_H
#define KIO_KEYDIR_H
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "common.h"
#include "entry.h"
#include "stats.h"

namespace bitcask
{
    struct ValueLocation;

    class KeyDir
    {
    public:
        // the keydir is the source of truth for the storage engine.
        // so it makes sens to let it hold the stats related to entries and datafiles
        struct Stats
        {
            struct FileStats
            {
                /// total bytes currently referenced by KeyDir
                uint64_t live_bytes{0};
                // number of live entries
                uint64_t live_records_count{0};
                // number of removed entries
                uint64_t tombstones_count{0};
            };

            // Map of file_id → FileStats
            std::unordered_map<uint64_t, FileStats> data_files;
            uint64_t data_files_count{0};  // optional, total number of files tracked
        };
        explicit KeyDir(size_t shard_count = kKeydirDefaultShardCount);

        /// Thread-safe put operations, replace it if exist
        void put(std::string&& key, const ValueLocation& loc) { shard_mut(key).put(std::move(key), loc); }

        [[nodiscard]] std::optional<ValueLocation> get(const std::string& key) const { return shard(key).get(key); }

        /// returns file_id and live bytes for compaction decision
        std::optional<std::pair<uint64_t, uint64_t>> remove(const std::string& key) { return shard_mut(key).remove(key); }

        /// Thread-safe put, only if the timestamp is newer.
        /// Used by the load_index process.
        void put_if_newer(std::string&& key, const ValueLocation& loc) { shard_mut(key).put_if_newer(std::move(key), loc); }

        /**
         * @brief Atomic Compare-And-Swap update for Compaction.
         * * Updates the entry for 'key' to 'new_loc' ONLY IF the current entry matches
         * 'expected_file_id' and 'expected_offset'.
         * * @return true if updated, false if the key was modified by someone else.
         */
        bool update_if_matches(const std::string& key, const ValueLocation& new_loc, uint64_t expected_file_id, uint64_t expected_offset)
        {
            return shard_mut(key).update_if_matches(key, new_loc, expected_file_id, expected_offset);
        }

        /**
         * @brief Creates a complete, point-in-time copy of the entire key directory.
         *
         * This method is the primary way to safely iterate over the index
         * without holding locks for an extended period. It works by
         * briefly taking a shared lock on each shard, copying its internal
         * map, and then releasing the lock.
         *
         * The returned map is a deep copy and is completely disconnected
         * from the live KeyDir. This allows the caller to perform long-running
         * operations (like merging, compaction, or hint file generation) without
         * blocking new 'put' or 'get' requests.
         *
         * @return A std::unordered_map containing a snapshot of all keys
         * and their locations at the time the call was made. It could be expensive to use
         * that method. See for_each_unlocked_shard.
         */
        [[nodiscard]]
        SimpleKeydir snapshot() const;  // NOSONAR

        /**
         * @brief Iterates over a snapshot of each shard, one shard at a time.
         *
         * This is the recommended way to perform long-running, read-only
         * operations like compaction or hint file generation.
         *
         * It avoids the high memory overhead of a full 'snapshot()' and the
         * high lock contention of a simple 'for_each()'.
         *
         * The provided callback 'fn' is called 'shard_count_' times.
         * Each time, it is given a 'std::unordered_map' containing a copy
         * of a single shard's data. The shard's lock is *released*
         * before your function is called.
         *
         * @param fn A function to be called for each shard's data.
         */
        template<typename Fn>
            requires std::invocable<Fn&, const SimpleKeydir&>
        void for_each_unlocked_shard(Fn&& fn) const
        {
            for (size_t i = 0; i < shard_count_; ++i)
            {
                SimpleKeydir shard_copy;
                {
                    std::shared_lock lock(shards_[i]->mu);
                    shard_copy = shards_[i]->index_;
                }

                std::forward<Fn>(fn)(shard_copy);
            }
        }

    private:
        struct Shard
        {
            // Keydir "sees" everything related to files, so it is the best place to collect the stats
            struct FileStats
            {
                /// bytes actually referenced by the keydir
                uint64_t live_bytes{0};
                uint64_t live_records_count{0};
                uint64_t tombstones_count{0};
            };

            std::unordered_map<uint64_t, FileStats> per_files_stats;
            mutable std::shared_mutex mu;
            std::unordered_map<std::string, ValueLocation, std::hash<std::string_view>, std::equal_to<>> index_;  // NOSONAR

            void put(std::string&& key, const ValueLocation& loc);
            [[nodiscard]] std::optional<ValueLocation> get(const std::string& key) const;
            void put_if_newer(std::string&& key, const ValueLocation& loc);
            std::optional<std::pair<uint64_t, uint64_t>> remove(const std::string& key);
            // CAS method
            bool update_if_matches(const std::string& key, const ValueLocation& new_loc, uint64_t expected_file_id, uint64_t expected_offset);
        };

        std::vector<std::unique_ptr<Shard>> shards_;
        // power of two
        std::size_t shard_count_{std::thread::hardware_concurrency()};
        [[nodiscard]] const Shard& shard(std::string_view key) const;
        Shard& shard_mut(std::string_view key) const;
        [[nodiscard]] Stats snapshot_stats() const;
    };
}  // namespace bitcask

#endif  // KIO_KEYDIR_H
