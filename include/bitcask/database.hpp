// #pragma once
//
#pragma once

#include "kio/kio.hpp"

#include <memory>

#include "bitcask/common.hpp"
#include <thread>
#include "bitcask/partition.hpp"

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
     * - Each partition has a dedicated IOWorker-
     * Keys distributed via consistent hashing
     * - Each partition is independent (share-nothing)
     */
    class Node;

    /**
     * @brief Main Bitcask database interface
     *
     * Facade that routes operations to the appropriate partition on the local node
     * or remote nodes.
     */
    class BitKV
    {
    public:
        /**
         * @brief Factory method to create and initialize database
         *
         * @param config Database configuration
         * @param partition_count Initial partition count
         * @return Initialized BitKV instance or error
         */
        static kio::Task<kio::Result<std::unique_ptr<BitKV>>> Open(const BitcaskConfig& config,
                                                                   size_t partition_count =
                                                                       std::thread::hardware_concurrency());

        // ====================================================================
        // CORE OPERATIONS
        // ====================================================================

        /**
         * @brief Put a key-value pair
         */
        kio::Task<kio::Result<void>> Put(std::string key, std::vector<std::byte> value) const;

        /**
         * @brief Get value for key
         */
        kio::Task<kio::Result<std::optional<std::vector<std::byte>>>> Get(std::string_view key) const;

        /**
         * @brief Delete key
         */
        kio::Task<kio::Result<void>> Del(std::string_view key) const;

        // ====================================================================
        // MANAGEMENT OPERATIONS
        // ====================================================================

        /**
         * @brief Force sync all partitions to disk
         */
        kio::Task<kio::Result<void>> Sync() const;

        kio::Task<kio::Result<>> Compact();

        /**
         * @brief Graceful shutdown
         */
        kio::Task<kio::Result<void>> Close() const;

        ~BitKV();

    private:
        // Private constructor - use open() factory
        BitKV(const BitcaskConfig& db_cfg, size_t partition_count);

        /**
         * @brief Map key to partition ID using hash modulo.
         */
        [[nodiscard]] uint32_t RouteToPartition(std::string_view key) const;

        BitcaskConfig db_config_;
        size_t partition_count_;
        std::vector<kio::IoContext> io_ctxs_;

        // Shutdown coordination
        std::atomic<bool> shutting_down_{true};
    };
} // namespace bitcask
