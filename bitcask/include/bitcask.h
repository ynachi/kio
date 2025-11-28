//
// Created by Yao ACHI on 20/11/2025.
//

#ifndef KIO_BITCASK_H
#define KIO_BITCASK_H
#include <memory>

#include "config.h"
#include "core/include/io/worker_pool.h"
#include "data_file.h"

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
        static kio::Task<kio::Result<std::unique_ptr<BitKV>>> open(BitcaskConfig config);

        // ====================================================================
        // CORE OPERATIONS
        // ====================================================================

        /**
         * @brief Put key-value pair
         * Routes to partition based on key hash
         */
        kio::Task<kio::Result<void>> put(std::string key, std::string value);

        /**
         * @brief Get value for key
         * Routes to partition based on key hash
         */
        kio::Task<kio::Result<std::optional<std::vector<char>>>> get(const std::string& key);

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
        kio::Task<kio::Result<void>> sync();

        /**
         * @brief Force compaction on all partitions
         */
        kio::Task<kio::Result<void>> compact();

        /**
         * @brief Graceful shutdown
         *
         * Steps:
         * 1. Stop accepting new operations
         * 2. Flush all active files
         * 3. Stop compaction loops
         * 4. Close all partitions
         * 5. Shutdown IOPool
         */
        kio::Task<kio::Result<void>> close();

        // ====================================================================
        // OBSERVABILITY
        // ====================================================================

        /**
         * @brief Get aggregated stats from all partitions
         */
        DatabaseStats get_stats() const;

        /**
         * @brief Export stats as Prometheus metrics
         */
        std::string export_prometheus_metrics() const;

        /**
         * @brief Get per-partition stats (for debugging)
         */
        std::vector<PartitionStats> get_partition_stats() const;

        /**
         * @brief Health check
         * Returns false if any partition is unhealthy
         */
        bool is_healthy() const;

        /**
         * @brief Get database info
         */
        struct DatabaseInfo
        {
            size_t partition_count;
            size_t total_files;
            uint64_t total_size_bytes;
            uint64_t total_live_bytes;
            double overall_fragmentation;
        };
        DatabaseInfo get_info() const;

        ~BitKV();

    private:
        // Private constructor - use open() factory
        BitKV(BitcaskConfig config, std::unique_ptr<kio::io::IOPool> io_pool);

        // ====================================================================
        // INITIALIZATION & RECOVERY
        // ====================================================================

        /**
         * @brief Initialize a database from scratch or recover existing
         */
        kio::Task<kio::Result<void>> initialize();

        /**
         * @brief Ensure directory structure exists
         * Creates: data/partition_0/, data/partition_1/, etc.
         */
        kio::Task<kio::Result<void>> ensure_directories();

        /**
         * @brief Detect if partition count changed since last run
         * Checks for partition directories that don't match the current config
         */
        kio::Task<kio::Result<bool>> partition_count_changed();

        /**
         * @brief Migrate data when partition count changes
         *
         * Process:
         * 1. Read all keys from old partitions
         * 2. Rehash each key with a new partition count
         * 3. Move data files if needed
         * 4. Rebuild hint files
         *
         * Note: This is complex and can be deferred for V1
         * (just error out if partition count changes)
         */
        kio::Task<kio::Result<void>> migrate_partitions(
            size_t old_partition_count,
            size_t new_partition_count);

        /**
         * @brief Recover all partitions in parallel
         */
        kio::Task<kio::Result<void>> recover_all_partitions();

        /**
         * @brief Start compaction loops for all partitions
         */
        void start_compaction_loops();

        // ====================================================================
        // ROUTING
        // ====================================================================

        /**
         * @brief Map key to partition ID
         * Uses consistent hashing: hash(key) % partition_count
         */
        size_t route_to_partition(std::string_view key) const;

        /**
         * @brief Get partition by ID
         */
        Partition& get_partition(size_t partition_id);
        const Partition& get_partition(size_t partition_id) const;

        // ====================================================================
        // MEMBERS
        // ====================================================================

        BitcaskConfig config_;
        std::unique_ptr<kio::io::IOPool> io_pool_;
        std::vector<std::unique_ptr<Partition>> partitions_;

        // Shutdown coordination
        std::atomic<bool> accepting_operations_{true};
        std::vector<kio::DetachedTask> compaction_tasks_;
    };

    // ========================================================================
    // AGGREGATED STATS
    // ========================================================================

    /**
     * @brief Aggregated statistics across all partitions
     */
    struct DatabaseStats
    {
        // Per-partition stats
        std::vector<PartitionStats> partitions;

        // Aggregated totals
        uint64_t total_puts{0};
        uint64_t total_gets{0};
        uint64_t total_gets_miss{0};
        uint64_t total_deletes{0};

        uint64_t total_compactions{0};
        uint64_t total_compactions_failed{0};
        uint64_t total_bytes_reclaimed{0};

        uint64_t total_files{0};
        uint64_t total_live_bytes{0};
        uint64_t total_dead_bytes{0};

        // FD cache stats (aggregated)
        struct AggregatedCacheStats
        {
            uint64_t total_hits{0};
            uint64_t total_misses{0};
            uint64_t total_evictions{0};
            double avg_hit_rate{0.0};
        } fd_cache;

        // Computed metrics
        [[nodiscard]] double overall_fragmentation() const
        {
            return total_live_bytes + total_dead_bytes > 0
                ? static_cast<double>(total_dead_bytes) / (total_live_bytes + total_dead_bytes)
                : 0.0;
        }

        [[nodiscard]] double cache_hit_rate() const
        {
            return fd_cache.total_hits + fd_cache.total_misses > 0
                ? static_cast<double>(fd_cache.total_hits) / (fd_cache.total_hits + fd_cache.total_misses)
                : 0.0;
        }

        /**
         * @brief Export as Prometheus format
         *
         * Example output:
         * ```
         * # HELP bitcask_puts_total Total number of put operations
         * # TYPE bitcask_puts_total counter
         * bitcask_puts_total 12345
         *
         * # HELP bitcask_gets_total Total number of get operations
         * # TYPE bitcask_gets_total counter
         * bitcask_gets_total{partition="0"} 5000
         * bitcask_gets_total{partition="1"} 4500
         * ...
         * ```
         */
        std::string to_prometheus() const;

        /**
         * @brief Export as JSON (for REST APIs)
         */
        std::string to_json() const;

        /**
         * @brief Human-readable summary
         */
        std::string to_string() const;
    };

    // ========================================================================
    // STATS COLLECTOR
    // ========================================================================

    /**
     * @brief Helper to collect and aggregate stats from all partitions
     */
    class StatsCollector
    {
    public:
        static DatabaseStats collect(const std::vector<std::unique_ptr<Partition>>& partitions);

    private:
        static void aggregate_partition_stats(
            DatabaseStats& db_stats,
            const PartitionStats& partition_stats);

        static void aggregate_cache_stats(
            DatabaseStats::AggregatedCacheStats& agg,
            const FdCache::Stats& cache_stats);
    };

}  // namespace bitcask

#endif  // KIO_BITCASK_H
