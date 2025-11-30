//
// Created by Yao ACHI on 20/11/2025.
//

#ifndef KIO_BITCASK_H
#define KIO_BITCASK_H
#include <memory>

#include "bitcask/include/partition.h"
#include "config.h"
#include "kio/include/io/worker_pool.h"

namespace bitcask
{
    /**
     * @brief Main Bitcask database interface
     *
     * Manages multiple partitions and routes operations based on key hash.
     * Each partition runs on its own dedicated worker thread.
     *
     * Architecture:
     * - N partitions (configurable, typically = CPU cores)
     * - Each partition has dedicated IOWorker
     * - Keys distributed via consistent hashing
     * - Each partition is independent (share-nothing)
     */
    class BitKV
    {
    public:
        /**
         * @brief Factory method to create and initialize database
         *
         * Steps:
         * 1. Validate config
         * 2. Create IOPool
         * 3. Ensure directory structure exists
         * 4. Handle partition migration if count changed
         * 5. Recover all partitions
         * 6. Start compaction loops
         *
         * @param config Database configuration
         * @return Initialized BitKV instance or error
         */
        static kio::Task<kio::Result<std::unique_ptr<BitKV>>> open(const BitcaskConfig& config, kio::io::WorkerConfig io_config, size_t partition_count);

        // ====================================================================
        // CORE OPERATIONS
        // ====================================================================

        /**
         * @brief Put key-value pair
         * Routes to partition based on key hash
         */
        kio::Task<kio::Result<void>> put(std::string&& key, std::vector<char>&& value);

        /**
         * @brief Get value for key
         * Routes to partition based on key hash
         */
        kio::Task<kio::Result<std::optional<std::vector<char>>>> get(const std::string& key);
        /**
         * Get returns the value converted to string
         * @param key
         * @return
         */
        kio::Task<kio::Result<std::optional<std::string>>> get_string(const std::string& key);
        /**
         * Put a string. Internally converts the string to a vector of char.
         * @param key
         * @param value
         * @return
         */
        kio::Task<kio::Result<void>> put(std::string key, std::string value);

        /**
         * @brief Delete key
         * Routes to partition based on key hash
         */
        kio::Task<kio::Result<void>> del(const std::string& key);

        // ====================================================================
        // MANAGEMENT OPERATIONS
        // ====================================================================

        /**
         * @brief Force sync all partitions to disk
         */
        kio::Task<kio::Result<void>> sync() const;

        /**
         * @brief Force compaction on all partitions
         */
        kio::Task<kio::Result<void>> compact() const;

        /**
         * @brief Graceful shutdown
         *
         * Closes all partitions (async I/O operations).
         * The worker pool will be stopped in the destructor.
         *
         * Steps:
         * 1. Stop accepting new operations
         * 2. Flush all active files
         * 3. Stop compaction loops
         * 4. Close all partitions
         */
        kio::Task<kio::Result<void>> close() const;

        /**
         * @brief Destructor
         *
         * Stops the worker pool. Partitions should already be closed via close().
         * This is always safe to call from main thread.
         */
        ~BitKV();

    private:
        struct InitState
        {
            std::latch latch;
            std::atomic<bool> has_error{false};
            std::mutex error_mutex;
            std::optional<kio::Error> first_error;
            explicit InitState(const size_t count) : latch(static_cast<int>(count)) {}
        };

        // Private constructor - use open() factory
        BitKV(const BitcaskConfig& db_config, const kio::io::WorkerConfig& io_config, size_t partition_count);

        // ====================================================================
        // INITIALIZATION & RECOVERY
        // ====================================================================

        static kio::DetachedTask initialize_partition(BitKV& db, kio::io::Worker&, InitState& state);

        /**
         * @brief Ensure directory structure exists
         * Creates: data/partition_0/, data/partition_1/, etc.
         */
        void ensure_directories() const;
        void check_or_create_manifest() const;

        // ====================================================================
        // ROUTING
        // ====================================================================

        /**
         * @brief Map key to partition ID
         * Uses consistent hashing: hash(key) % partition_count
         */
        [[nodiscard]] size_t route_to_partition(std::string_view key) const;

        /**
         * @brief Get partition by ID
         */
        Partition& get_partition(size_t partition_id);

        // ====================================================================
        // MEMBERS
        // ====================================================================

        BitcaskConfig db_config_;
        kio::io::WorkerConfig io_config_;
        std::unique_ptr<kio::io::IOPool> io_pool_;
        // mutex used during initialization only. Each partition will be created on a different
        // thread to parallelize recovery, then will be pushed to the partition list.
        std::mutex partition_mu_;
        std::vector<std::unique_ptr<Partition>> partitions_;
        size_t partition_count_;

        // Shutdown coordination
        std::atomic<bool> accepting_operations_{true};
    };

}  // namespace bitcask

#endif  // KIO_BITCASK_H
