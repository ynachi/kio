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
    /// Value location in data file
    struct ValueLocation
    {
        std::uint32_t file_id;
        uint64_t offset;
        uint32_t value_size;
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

        // Snapshot for hint file generation
        // TODO: not sure needed
        [[nodiscard]]
        std::unordered_map<std::string, ValueLocation> snapshot() const;

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
