//
// Created by Yao ACHI on 14/11/2025.
//

#ifndef KIO_STATS_H
#define KIO_STATS_H
#include <cstdint>
#include <unordered_map>

namespace bitcask
{
    struct PartitionStats
    {
        struct FileStats
        {
            uint64_t total_bytes{0};
            uint64_t live_bytes{0};
            // number of live entries
            uint64_t live_entries{0};
            // number of removed/overwritten entries
            uint64_t dead_entries{0};

            [[nodiscard]] double fragmentation() const { return total_bytes > 0 ? 1.0 - static_cast<double>(live_bytes) / static_cast<double>(total_bytes) : 0.0; }
        };

        // Map of file_id → FileStats
        std::unordered_map<uint64_t, FileStats> data_files;

        // Files
        uint32_t sealed_files_count;  // sealed_file_fds_.size()
        uint64_t active_file_bytes;  // active_file_->size()

        // Compaction state
        bool compaction_running;

        // Basic ops (monotonic counters)
        uint64_t puts_total;
        uint64_t gets_total;
        uint64_t gets_miss_total;
        uint64_t deletes_total;

        // Compaction events
        uint64_t compactions_total;
        uint64_t compactions_failed;
        uint64_t bytes_reclaimed_total;  // Space freed by compaction

        // File lifecycle
        uint64_t file_rotations_total;
        uint64_t files_compacted_total;  // Source files processed
    };
}  // namespace bitcask

#endif  // KIO_STATS_H
