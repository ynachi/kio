#pragma once


#include "partition_recovery.hpp"
#include "kio/kio.hpp"
#include "absl/container/flat_hash_set.h"
#include "bitcask/compactor.hpp"
#include "bitcask/files.hpp"

namespace bitcask
{
    class Partition
    {
    public:
        // Initialization
        // TODO: make the constructor private and use the factory method
        // Make it start background sync job of the active file if needed
        // also make it start the compaction loop if needed.
        static kio::Task<kio::Result<std::unique_ptr<Partition>>> Open(
            kio::IoContext& ctx,
            BitcaskConfig& config,
            size_t partition_id
        );

        // Delegate operations
        kio::Task<kio::Result<void>> Put(kio::IoContext& ctx, std::string key, std::span<const std::byte> value)
        {
            return io_.Put(ctx, std::move(key), value);
        }

        kio::Task<kio::Result<std::optional<std::vector<std::byte>>>> Get(
            kio::IoContext& ctx,
            std::string_view key
        )
        {
            return io_.Get(ctx, key);
        }

        kio::Task<kio::Result<void>> Del(kio::IoContext& ctx, std::string key)
        {
            return io_.Del(ctx, std::move(key));
        }

        kio::Task<kio::Result<void>> Compact(kio::IoContext& ctx)
        {
            return compactor_.Compact(ctx);
        }

        kio::Task<kio::Result<void>> AsyncClose(kio::IoContext& ctx);

        size_t GetID() const { return partition_id_; }
        kio::TaskGroup<>& BgJobs() { return bg_jobs; }

    private:
        PartitionIO io_;
        Compactor compactor_;
        BitcaskConfig config_;
        size_t partition_id_;
        PartitionStats stats_{};
        kio::TaskGroup<> bg_jobs{2};

        Partition(BitcaskConfig& config, size_t partition_id)
            : io_(config, partition_id, stats_), compactor_(io_, config, stats_)
              , config_(config)
              , partition_id_(partition_id)
        {
        }
    };
} // namespace bitcask
