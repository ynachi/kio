//
// Created by Yao ACHI on 21/11/2025.
//

#ifndef KIO_BITCASK_PARTITION_H
#define KIO_BITCASK_PARTITION_H

#include "compactor.h"
#include "data_file.h"
#include "entry.h"
#include "fd_cache.h"
#include "file_id.h"
#include "kio/sync/baton.h"

namespace bitcask
{
/**
 * @brief Partition is a self-contained bitcast DB engine. It's a building block of the main DB. It does not take
 * ownership of the worker and the configs. It is not meant to be used directly, as a standalone unit. But if for some
 * reason, you do use it this way, make sure the worker and the configs outlive any bitcask operation. One way to do
 * that is to not exit your main thread until every DB work is done.
 */
class Partition
{
    // Memory Pool: Must be declared BEFORE keydir_ so it is initialized first
    // We use unsynchronized_pool_resource because this partition runs on a single thread (shared-nothing).
    std::pmr::unsynchronized_pool_resource mem_pool_;

    // The KeyDir uses the pool
    Keydir keydir_;
    kio::io::Worker& worker_;
    std::unique_ptr<DataFile> active_file_;
    FDCache fd_cache_;
    FileIdGenerator file_id_gen_;

    BitcaskConfig config_{};
    size_t partition_id_;
    PartitionStats stats_{};

    kio::sync::AsyncBaton compaction_trigger_;
    compactor::CompactionLimits compaction_limits_{};
    std::atomic<bool> shutting_down_{false};

    // Helper methods
    kio::Task<kio::Result<DataEntry>> AsyncReadEntry(int fd, uint64_t offset, uint32_t size) const;
    kio::Task<kio::Result<void>> RotateActiveFile();
    kio::Task<kio::Result<int>> FindFd(uint64_t file_id);

    // Start the loop in the background if a compaction flag is set
    kio::DetachedTask CompactionLoop();
    // background sync job for the active file, in case sync_on_write is not set
    kio::DetachedTask BackgroundSync();
    void SignalCompaction(uint64_t file_id);
    bool ShouldCompactFile(uint64_t file_id) const;
    std::vector<uint64_t> FindFragmentedFiles() const;
    std::filesystem::path GetDataFilePath(uint64_t file_id) const;
    std::filesystem::path GetHintFilePath(uint64_t file_id) const;

    // recovery
    /**
     * @brief Scan directory and collect all data file IDs
     *
     * Returns a sorted list (oldest to newest) using file_id_compare_by_time
     */
    std::vector<uint64_t> ScanDataFiles() const;
    // read hint file to build keydir, the caller have to make sure the file exists
    /**
     * @brief Recover a single file using a hint file (fast path)
     *
     * @param fh The file to recover
     * @param file_id
     */
    kio::Task<kio::Result<void>> RecoverFromHintFile(const FileHandle& fh, uint64_t file_id);
    kio::Task<kio::Result<void>> RecoverFromDataFile(const FileHandle& fh, uint64_t file_id);
    // returns recovered and skipped entries
    kio::Result<std::pair<uint64_t, uint64_t>> RecoverDataFromBuffer(kio::BytesMut& buffer, uint64_t file_id,
                                                                     uint64_t file_offset = 0);
    // recover the partition and runtime stats
    kio::Task<kio::Result<void>> Recover();
    kio::Task<bool> TryRecoverFromHint(uint64_t file_id);
    kio::Task<kio::Result<void>> CreateAndSetActiveFile();
    /**
     * Seal the active file, truncate it to the actual db size.
     * This method invalidates the active file.
     * @return
     */
    kio::Task<kio::Result<void>> SealActiveFile();

    // private constructors
    Partition(const BitcaskConfig& config, kio::io::Worker& worker, size_t partition_id);

public:
    /**
     * Creates a Bitcask partition.
     * @param config Bitcask configuration
     * @param worker The non-owning worker to be used by this partition
     * @param partition_id
     * @return an unique ptr to the partition
     *
     * @note This method does not create the directory for the partition. The call MUST ensure it exist before.
     */
    static kio::Task<kio::Result<std::unique_ptr<Partition>>> Open(const BitcaskConfig& config, kio::io::Worker& worker,
                                                                   size_t partition_id);
    kio::Task<kio::Result<void>> AsyncClose();
    // cannot copy and cannot move
    Partition(const Partition&) = delete;
    Partition& operator=(const Partition&) = delete;
    Partition(Partition&&) = delete;
    Partition& operator=(Partition&&) = delete;
    //****************************************

    // Core Operations
    kio::Task<kio::Result<void>> Put(std::string&& key, std::vector<char>&& value);
    kio::Task<kio::Result<std::optional<std::vector<char>>>> Get(std::string_view key);
    kio::Task<kio::Result<void>> Del(std::string_view key);

    // Secondary operations

    /**
     * @brief Force any write operation to flush to disk
     * @return
     */
    // TODO: implement
    kio::Task<kio::Result<void>> Sync();
    // force manual compact, scans all the old file and compact if possible
    // TODO implement
    kio::Task<kio::Result<void>> Compact();
    [[nodiscard]] const PartitionStats& GetStats() const { return stats_; }
    [[nodiscard]] const FDCache::Stats& GetCacheStats() const { return fd_cache_.GetStats(); }
    [[nodiscard]] std::filesystem::path DbPath() const
    {
        return config_.directory / std::format("partition_{}", partition_id_);
    }
    [[nodiscard]] uint64_t PartitionId() const { return partition_id_; }
    [[nodiscard]] uint64_t ActiveFileId() const { return active_file_->FileId(); }
};
}  // namespace bitcask

#endif  // KIO_BITCASK_PARTITION_H
