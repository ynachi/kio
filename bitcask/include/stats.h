//
// Created by Yao ACHI on 14/11/2025.
//

#ifndef KIO_STATS_H
#define KIO_STATS_H
#include <cstdint>
#include <ranges>
#include <unordered_map>

namespace bitcask
{
struct PartitionStats
{
    struct FileStats
    {
        uint64_t total_bytes{0};
        uint64_t live_bytes{0};
        uint64_t live_entries{0};

        [[nodiscard]] double fragmentation() const
        {
            return total_bytes > 0 ? 1.0 - static_cast<double>(live_bytes) / static_cast<double>(total_bytes) : 0.0;
        }

        [[nodiscard]] uint64_t reclaimable_bytes() const { return total_bytes - live_bytes; }
    };

    // Per-file stats
    std::unordered_map<uint64_t, FileStats> data_files;

    // Compaction state
    bool compaction_running{false};

    // Runtime operation counters
    uint64_t puts_total{0};
    uint64_t gets_total{0};
    uint64_t gets_miss_total{0};
    uint64_t deletes_total{0};  // total of deletion requests

    // Compaction metrics
    uint64_t compactions_total{0};
    uint64_t compactions_failed{0};
    uint64_t bytes_reclaimed_total{0};
    uint64_t files_compacted_total{0};

    // File lifecycle
    uint64_t file_rotations_total{0};

    [[nodiscard]] uint64_t total_reclaimable_bytes() const
    {
        uint64_t total = 0;
        for (const auto& stats: data_files | std::views::values)
        {
            total += stats.reclaimable_bytes();
        }
        return total;
    }

    [[nodiscard]] double overall_fragmentation() const
    {
        uint64_t total_bytes = 0;
        uint64_t live_bytes = 0;
        for (const auto& stats: data_files | std::views::values)
        {
            total_bytes += stats.total_bytes;
            live_bytes += stats.live_bytes;
        }

        return total_bytes > 0 ? 1.0 - static_cast<double>(live_bytes) / static_cast<double>(total_bytes) : 0.0;
    }
};
}  // namespace bitcask

#endif  // KIO_STATS_H
