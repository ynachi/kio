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
public:
    /**
     * Creates a Bitcask partition.
     * @param config Bitcask configuration
     * @param worker The non-owning worker to be used by this partition
     * @param partition_id
     * @return a unique ptr to the partition
     *
     * @note This method does not create the directory for the partition. The call MUST ensure it exist before.
     */
    static kio::Task<kio::Result<std::unique_ptr<Partition>>> Open(const BitcaskConfig& config, kio::io::Worker& worker,
                                                                   size_t partition_id);

    // ctor/dtor
    Partition(const BitcaskConfig& config, kio::io::Worker& worker, size_t partition_id);
    // cannot copy and cannot move
    // Partition(const Partition&) = delete;
    // Partition& operator=(const Partition&) = delete;
    // Partition(Partition&&) = delete;
    // Partition& operator=(Partition&&) = delete;

    // Core Operations
    kio::Task<kio::Result<void>> Put(std::string&& key, std::vector<char>&& value);
    kio::Task<kio::Result<std::optional<std::vector<char>>>> Get(std::string_view key);
    kio::Task<kio::Result<void>> Del(std::string_view key);

    // Maintenance
    kio::Task<kio::Result<void>> Sync() const;
    kio::Task<kio::Result<void>> Compact();
    kio::Task<kio::Result<void>> AsyncClose();

    // secondary operations
    [[nodiscard]] const PartitionStats& GetStats() const { return stats_; }
    [[nodiscard]] const FDCache::Stats& GetCacheStats() const { return fd_cache_.GetStats(); }
    [[nodiscard]] uint64_t PartitionId() const { return partition_id_; }

private:
    // Members
    KeyDir keydir_;  // Now uses absl::flat_hash_map, no PMR needed

    kio::io::Worker& worker_;
    std::unique_ptr<DataFile> active_file_;
    FDCache fd_cache_;
    FileIdGenerator file_id_gen_;

    BitcaskConfig config_;
    size_t partition_id_;
    PartitionStats stats_{};

    // Compaction control
    kio::sync::AsyncBaton compaction_trigger_;
    compactor::CompactionLimits compaction_limits_;
    std::atomic<bool> shutting_down_{false};

    // Internal Helpers
    kio::Task<kio::Result<void>> Recover();
    kio::Task<kio::Result<void>> RecoverFromDataFile(const FileHandle& fh, uint64_t file_id);
    kio::Task<bool> TryRecoverFromHint(uint64_t file_id);
    kio::Task<kio::Result<void>> RecoverFromHintFile(const FileHandle& fh, uint64_t file_id);

    kio::Result<std::pair<uint64_t, uint64_t>> RecoverDataFromBuffer(kio::BytesMut& buffer, uint64_t file_id,
                                                                     uint64_t file_offset);

    kio::Task<kio::Result<void>> CreateAndSetActiveFile();
    kio::Task<kio::Result<void>> RotateActiveFile();
    /**
     * Seal the active file, truncate it to the actual db size.
     * This method invalidates the active file.
     * @return
     */
    kio::Task<kio::Result<void>> SealActiveFile();

    kio::Task<kio::Result<int>> FindFd(uint64_t file_id);
    kio::Task<kio::Result<DataEntry>> AsyncReadEntry(int fd, uint64_t offset, uint32_t size) const;

    void SignalCompaction(uint64_t file_id);
    bool ShouldCompactFile(uint64_t file_id) const;
    std::vector<uint64_t> FindFragmentedFiles() const;
    kio::DetachedTask CompactionLoop();

    kio::DetachedTask BackgroundSync();

    [[nodiscard]] std::vector<uint64_t> ScanDataFiles() const;
    [[nodiscard]] std::filesystem::path DbPath() const { return config_.directory; }
    [[nodiscard]] std::filesystem::path GetDataFilePath(uint64_t file_id) const;
    [[nodiscard]] std::filesystem::path GetHintFilePath(uint64_t file_id) const;

    [[nodiscard]] uint64_t ActiveFileId() const { return active_file_ ? active_file_->FileId() : 0; }
};
}  // namespace bitcask

#endif  // KIO_BITCASK_PARTITION_H
