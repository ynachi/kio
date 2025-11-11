//
// Created by Yao ACHI on 08/11/2025.
//

#ifndef KIO_KEYDIR_H
#define KIO_KEYDIR_H
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace bitcask
{
    // KeyDir entry (in-memory index):
    // +-----------+-------------+-------------+--------------+
    // | file_id(4)| offset(8)   | total_sz(8) | timestamp(8)*|
    // +-----------+-------------+-------------+--------------+
    // *timestamp optional (used for conflict resolution / merge)
    //
    // Maps: key → KeyDirEntry
    // file_id: Which data file the entry resides in
    // offset: Where [CRC|SIZE|PAYLOAD] starts in that file
    // total_sz: 4 + 8 + payload_size (total entry bytes on disk)

    /// Value location in data file
    struct ValueLocation
    {
        std::uint32_t file_id;
        uint64_t offset;
        uint32_t total_size;
        uint64_t timestamp;
    };

    class KeyDir
    {
    public:
        explicit KeyDir(size_t shard_count = 4);
        /// Thread-safe put operations, replace it if exist
        void put(std::string&& key, ValueLocation loc) { shard(key).put(std::move(key), loc); }
        [[nodiscard]]
        std::optional<ValueLocation> get(const std::string& key) const
        {
            return shard(key).get(key);
        }

        /// Thread-safe put, only if timestamp is newer.
        /// Used by the load_index process.
        void put_if_newer(std::string&& key, ValueLocation loc) { shard(key).put_if_newer(std::move(key), loc); }

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
        std::unordered_map<std::string, ValueLocation> snapshot() const;

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
        void for_each_unlocked_shard(std::function<void(const std::unordered_map<std::string, ValueLocation>&)> fn) const;

    private:
        struct Shard
        {
            std::shared_mutex mu;
            std::unordered_map<std::string, ValueLocation> index_;

            void put(std::string&& key, ValueLocation loc);
            [[nodiscard]]
            std::optional<ValueLocation> get(const std::string& key);
            void put_if_newer(std::string&& key, ValueLocation loc);
        };

        std::vector<std::unique_ptr<Shard>> shards_;
        // power of two
        std::uint32_t shard_count_{4};
        [[nodiscard]]
        Shard& shard(std::string_view key) const;
    };
}  // namespace bitcask

#endif  // KIO_KEYDIR_H
