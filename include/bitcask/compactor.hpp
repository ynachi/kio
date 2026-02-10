#pragma once
#include <vector>

#include "partition_io.hpp"
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
    // Compactor - Compaction Logic
    // ===================================================================
    class Compactor
    {
    public:
        Compactor(PartitionIO& io, BitcaskConfig& config, PartitionStats& stats) :
            io_(io), config_(config), stats_(stats)
        {
        }

        void RequestStop()
        {
            shutting_down_.store(true, std::memory_order_release);
        }

        // Compaction operations
        kio::Task<kio::Result<void>> Compact(kio::IoContext& ctx);

        kio::Task<kio::Result<CompactionResult>> CompactFiles(
            kio::IoContext& ctx,
            const std::vector<uint64_t>& src_file_ids,
            DataFile& dst_file
        );

        kio::Task<> CompactionLoop(kio::IoContext& ctx);

        // Analysis
        std::vector<uint64_t> FindFragmentedFiles() const;
        bool ShouldCompactFile(uint64_t file_id) const;

    private:
        PartitionIO& io_;
        BitcaskConfig& config_;
        PartitionStats& stats_;

        std::atomic<bool> shutting_down_{false};
        std::atomic<bool> compaction_running_{false};

        // --- Helper Structures & Methods ---

        struct CompactionContext
        {
            CompactionResult result;
            uint64_t current_src_id = 0;
            uint64_t current_src_offset = 0; // Logical offset in a current source file
            uint64_t dst_write_offset = 0; // Physical offset in destination
            uint64_t dst_logical_offset = 0; // Logical offset for the next entry
        };

        // Processes entries currently in the buffer.
        // Returns success or error if writing fails.
        kio::Task<kio::Result<void>> ProcessBufferEntries(
            kio::IoContext& ctx,
            kio::IoBuffer& in_buf,
            kio::IoBuffer& out_buf,
            DataFile& dst_file,
            CompactionContext& c_ctx,
            bool is_eof
        );

        // Updates the in-memory KeyDir with the new locations after compaction
        void UpdateKeyDir(const std::vector<HintEntry>& new_hints, uint64_t dst_file_id) const;

        // Deletes the old source files after successful compaction
        kio::Task<kio::Result<uint64_t>> DeleteSourceFiles(
            kio::IoContext& ctx,
            const std::vector<uint64_t>& fragmented_files
        );
    };
} // namespace bitcask
