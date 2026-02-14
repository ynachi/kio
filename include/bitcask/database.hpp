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
        static constexpr size_t kPartitionCount = CLUSTER_PARTITION_COUNT;
        static constexpr bool kPartitionCountIsPow2 = (kPartitionCount & (kPartitionCount - 1)) == 0;
        static constexpr size_t kPartitionMask = kPartitionCount - 1;

        struct RouteTarget
        {
            uint16_t partition_id;
            Partition& partition;
            kio::IoContext& ctx;
        };

        // Fast hot-path route: key -> (partition, io_context)
        [[nodiscard]] RouteTarget Route(std::string_view key) const noexcept
        {
            const uint16_t pid = ComputePartitionId(key);
            const RouteSlot& slot = route_[pid];

            assert(slot.partition != nullptr);
            assert(slot.ctx != nullptr);

            return RouteTarget{
                .partition_id = pid,
                .partition = *slot.partition,
                .ctx = *slot.ctx,
            };
        }

        /**
         * @brief Factory method to create and initialize a database
         *
         * @param config Database configuration
         * @param io_worker_count
         * @return Initialized BitKV instance or error
         */
        static kio::Task<kio::Result<std::unique_ptr<BitKV>>> Open(BitcaskConfig& config,
                                                                   size_t io_worker_count =
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
        kio::Task<kio::Result<void>> Del(std::string key) const;

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
        BitKV(const BitcaskConfig& config, size_t io_worker_count) : config_(config), io_worker_count_(io_worker_count),
                                                                     partitions_(kPartitionCount),
                                                                     io_ctxs_(io_worker_count),
                                                                     start_latch_(io_worker_count)
        {
        }

        void StartIoThreads(size_t io_worker_count);

        kio::Task<kio::Result<>> CreatePartitions();
        void BuildRoutes();

        struct RouteSlot
        {
            Partition* partition{nullptr};
            kio::IoContext* ctx{nullptr};
        };

        static uint16_t ComputePartitionId(std::string_view key) noexcept
        {
            const uint64_t h = Hash(key);

            if constexpr (kPartitionCountIsPow2)
            {
                return static_cast<uint16_t>(h & kPartitionMask); // fastest path
            }
            else
            {
                return static_cast<uint16_t>(h % kPartitionCount); // safe fallback
            }
        }

        std::array<RouteSlot, kPartitionCount> route_{};

        BitcaskConfig config_;
        size_t io_worker_count_;

        // Ownership / lifetime
        std::vector<std::unique_ptr<Partition>> partitions_;
        std::vector<std::unique_ptr<kio::IoContext>> io_ctxs_;
        std::vector<std::jthread> io_threads_;
        // non-owning, initialized at startup
        std::array<kio::IoContext*, kPartitionCount> ctx_lookup_{};

        // Shutdown coordination
        std::atomic<bool> shutting_down_{false};
        std::latch start_latch_;
    };
} // namespace bitcask
