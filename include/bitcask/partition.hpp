#pragma once


#include "kio/kio.hpp"
#include "absl/container/flat_hash_set.h"
#include "bitcask/compactor.hpp"
#include "bitcask/files.hpp"

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
        std::vector<HintEntry> new_hints; // To update KeyDir atomically later
    };

    struct CompactionLimits
    {
        size_t max_compact_size = 1024 * 1024 * 1024; // 1GB
        double min_fragmentation = 0.3;
    };

    /**
     * @brief Partition is a self-contained bitcast DB engine. It's a building block of the main DB. It does not take
     * ownership of the worker and the configs. It is not meant to be used directly, as a standalone unit. But if for some
     * reason, you do use it this way, make sure the worker and the configs outlive any bitcask operation. One way to do
     * that is to not exit your main thread until every DB work is done.
     */
    class Partition
    {
    public:
        // ctor/dtor
        Partition(const BitcaskConfig& config, size_t partition_id);
        // cannot copy and cannot move
        // Partition(const Partition&) = delete;
        // Partition& operator=(const Partition&) = delete;
        // Partition(Partition&&) = delete;
        // Partition& operator=(Partition&&) = delete;

        // Core Operations
        kio::Task<kio::Result<void>> Put(kio::IoContext& ctx, std::string key, std::span<const std::byte> value);
        kio::Task<kio::Result<std::optional<std::vector<std::byte>>>> Get(kio::IoContext& ctx, std::string_view key);
        kio::Task<kio::Result<void>> Del(kio::IoContext& ctx, std::string key);

        // Maintenance
        kio::Task<kio::Result<void>> Compact(kio::IoContext& ctx);
        // TODO, keep it public for now, because I need to test it as its logic is fairly complex
        kio::Task<kio::Result<CompactionResult>> CompactFiles(kio::IoContext& ctx,
                                                              const std::vector<uint64_t>& src_file_ids,
                                                              DataFile& dst_file);
        kio::Task<kio::Result<void>> AsyncClose(kio::IoContext& ctx);

        // secondary operations
        [[nodiscard]] const PartitionStats& GetStats() const { return stats_; }
        [[nodiscard]] const FDCache::Stats& GetCacheStats() const { return fd_cache_.GetStats(); }
        [[nodiscard]] uint64_t PartitionId() const { return partition_id_; }

        // Background tasks: Started explicitly with context
        void StartBackgroundTasks(kio::IoContext& ctx);

    private:
        // Background task implementations
        kio::Task<> CompactionLoop(kio::IoContext& ctx);
        kio::Task<> BackgroundSync(kio::IoContext& ctx);

        // Members
        KeyDir keydir_;

        std::unique_ptr<DataFile> active_file_;
        FDCache fd_cache_;
        FileIdGenerator file_id_gen_;

        BitcaskConfig config_;
        size_t partition_id_;
        PartitionStats stats_{};

        // Compaction control
        // TODO: change me
        // kio::sync::AsyncBaton compaction_trigger_;
        // kio::sync::AsyncBaton compaction_stop_;
        // kio::sync::AsyncBaton sync_job_stop_;
        kio::Notifier compaction_signal_;
        absl::flat_hash_set<uint64_t> compaction_candidates_;
        std::atomic<bool> shutting_down_{false};
        std::atomic_bool compaction_running_{false};

        // Internal Helpers
        kio::Task<kio::Result<void>> Recover();
        kio::Task<kio::Result<void>> RecoverFromDataFile(const kio::FDGuard& fh, uint64_t file_id);
        kio::Task<bool> TryRecoverFromHint(uint64_t file_id);
        kio::Task<kio::Result<void>> RecoverFromHintFile(const kio::FDGuard& fh, uint64_t file_id);

        kio::Result<std::pair<uint64_t, uint64_t>> RecoverDataFromBuffer(kio::IoBuffer& buffer, uint64_t file_id,
                                                                         uint64_t file_read_position);

        kio::Task<kio::Result<void>> CreateAndSetActiveFile(kio::IoContext& ctx);
        kio::Task<kio::Result<void>> RotateActiveFile(kio::IoContext& ctx);

        /**
         * @brief Write a hint file for a sealed data file.
         *
         * Hint files contain a compact index of all live keys in the data file,
         * enabling fast recovery without scanning the entire data file.
         *
         * @param file_id The ID of the data file being sealed
         * @return Result indicating success or failure
         */
        kio::Task<kio::Result<void>> WriteHintFile(kio::IoContext& ctx, uint64_t file_id);

        /**
         * Seal the active file, truncate it to the actual db size.
         * This method invalidates the active file.
         * @return
         */
        kio::Task<kio::Result<void>> SealActiveFile(kio::IoContext& ctx);

        kio::Task<kio::Result<DataEntry>> AsyncReadEntry(kio::IoContext& ctx, int fd, uint64_t offset,
                                                         uint32_t size) const;

        void SignalCompaction(uint64_t file_id);
        bool ShouldCompactFile(uint64_t file_id) const;
        std::vector<uint64_t> FindFragmentedFiles() const;
        kio::Task<> CompactionLoop();

        kio::Task<> BackgroundSync();

        [[nodiscard]] std::vector<uint64_t> ScanDataFiles() const;
        [[nodiscard]] std::filesystem::path DbPath() const { return config_.directory; }
        [[nodiscard]] std::filesystem::path GetDataFilePath(uint64_t file_id) const;

        std::filesystem::path GetHintFilePath(uint64_t file_id) const
        {
            return config_.directory / std::format("partition_{}/hint_{}.ht", partition_id_, file_id);
        }

        [[nodiscard]] uint64_t ActiveFileId() const { return active_file_ ? active_file_->FileId() : 0; }
    };
} // namespace bitcask
