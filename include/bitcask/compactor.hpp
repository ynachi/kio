#pragma once
#include <cstdint>
#include <vector>

#include "partition_io.hpp"
#include "absl/container/flat_hash_set.h"
#include "kio/core/core.hpp"

namespace bitcask
{
    // TODO: parametrize those ?
    // Read Buffer: 64KB - 256KB (Large enough for standard entries)
    constexpr size_t kDefaultInBufReadSize = 256 * 1024;
    // Write Buffer: 4MB (Ideal for sequential disk writes)
    constexpr size_t kDefaultOutBufWriteSize = 4 * 1024 * 1024;

    struct CompactionResult
    {
        uint64_t bytes_reclaimed = 0;
        std::vector<HintEntry> new_hints;
    };

    // ===================================================================
    // 3. PartitionCompactor - Compaction Logic
    // ===================================================================
    class Compactor
    {
    public:
        Compactor(PartitionIO& io, BitcaskConfig& config, PartitionStats& stats) :
            io_(io), config_(config), stats_(stats)
        {
        }

        // Compaction operations
        kio::Task<kio::Result<void>> Compact(kio::IoContext& ctx);

        kio::Task<kio::Result<CompactionResult>> CompactFiles(
            kio::IoContext& ctx,
            const std::vector<uint64_t>& src_file_ids,
            DataFile& dst_file
        );

        // Background tasks
        void StartBackgroundTasks(kio::IoContext& ctx);
        kio::Task<> CompactionLoop(kio::IoContext& ctx);

        // Signaling
        void SignalCompaction(uint64_t file_id);
        // Analysis
        std::vector<uint64_t> FindFragmentedFiles() const;
        bool ShouldCompactFile(uint64_t file_id) const;

    private:
        PartitionIO& io_;
        BitcaskConfig& config_;
        PartitionStats& stats_;

        kio::Notifier compaction_signal_;
        absl::flat_hash_set<uint64_t> compaction_candidates_;
        std::atomic<bool> shutting_down_{false};
        std::atomic<bool> compaction_running_{false};
    };
} // namespace bitcask
