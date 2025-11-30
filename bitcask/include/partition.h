//
// Created by Yao ACHI on 21/11/2025.
//

#ifndef KIO_BITCASK_PARTITION_H
#define KIO_BITCASK_PARTITION_H

#include "compactor.h"
#include "core/include/sync/baton.h"
#include "data_file.h"
#include "entry.h"
#include "fd_cache.h"
#include "file_id.h"

namespace bitcask
{
    /**
     * @brief Partition is a self-contained bitcast DB engine. It's a building block of the main DB. It does not take ownership of
     * the worker and the configs. It is not meant to be used directly, as a standalone unit. But if for some reason, you do use it
     * this way, make sure the worker and the configs outlive any bitcask operation. One way to do that is to not exit your main
     * thread until every DB work is done.
     */
    class Partition
    {
        // Memory Pool: Must be declared BEFORE keydir_ so it is initialized first
        // We use unsynchronized_pool_resource because this partition runs on a single thread (shared-nothing).
        std::pmr::unsynchronized_pool_resource mem_pool_;

        // The KeyDir uses the pool
        SimpleKeydir keydir_;
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
        kio::Task<kio::Result<DataEntry>> async_read_entry(int fd, uint64_t offset, uint32_t size) const;
        kio::Task<kio::Result<void>> rotate_active_file();
        kio::Task<kio::Result<int>> find_fd(uint64_t file_id);

        // Start the loop in the background if a compaction flag is set
        kio::DetachedTask compaction_loop();
        // background sync job for the active file, in case sync_on_write is not set
        kio::DetachedTask background_sync();
        void signal_compaction(uint64_t file_id);
        bool should_compact_file(uint64_t file_id) const;
        std::vector<uint64_t> find_fragmented_files() const;
        std::filesystem::path get_data_file_path(uint64_t file_id) const;
        std::filesystem::path get_hint_file_path(uint64_t file_id) const;

        // recovery
        /**
         * @brief Scan directory and collect all data file IDs
         *
         * Returns a sorted list (oldest to newest) using file_id_compare_by_time
         */
        std::vector<uint64_t> scan_data_files() const;
        // read hint file to build keydir, the caller have to make sure the file exists
        /**
         * @brief Recover a single file using a hint file (fast path)
         *
         * @param fh The file to recover
         * @param file_id
         */
        kio::Task<kio::Result<void>> recover_from_hint_file(const FileHandle& fh, uint64_t file_id);
        kio::Task<kio::Result<void>> recover_from_data_file(const FileHandle& fh, uint64_t file_id);
        // returns recovered and skipped entries
        kio::Result<std::pair<uint64_t, uint64_t>> recover_data_from_buffer(kio::BytesMut& buffer, uint64_t file_id, uint64_t file_offset = 0);
        // recover the partition and runtime stats
        kio::Task<kio::Result<void>> recover();
        kio::Task<bool> try_recover_from_hint(uint64_t file_id);
        kio::Task<kio::Result<void>> create_and_set_active_file();
        /**
         * Seal the active file, truncate it to the actual db size.
         * This method invalidates the active file.
         * @return
         */
        kio::Task<kio::Result<void>> seal_active_file();

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
        static kio::Task<kio::Result<std::unique_ptr<Partition>>> open(const BitcaskConfig& config, kio::io::Worker& worker, size_t partition_id);
        kio::Task<kio::Result<void>> async_close();
        // cannot copy and cannot move
        Partition(const Partition&) = delete;
        Partition& operator=(const Partition&) = delete;
        Partition(Partition&&) = delete;
        Partition& operator=(Partition&&) = delete;
        //****************************************

        // Core Operations
        kio::Task<kio::Result<void>> put(std::string&& key, std::vector<char>&& value);
        kio::Task<kio::Result<std::optional<std::vector<char>>>> get(const std::string& key);
        kio::Task<kio::Result<void>> del(const std::string& key);

        // Secondary operations

        /**
         * @brief Force any write operation to flush to disk
         * @return
         */
        // TODO: implement
        kio::Task<kio::Result<void>> sync();
        // force manual compact, scans all the old file and compact if possible
        // TODO implement
        kio::Task<kio::Result<void>> compact();
        [[nodiscard]] const PartitionStats& get_stats() const { return stats_; }
        [[nodiscard]] const FDCache::Stats& get_cache_stats() const { return fd_cache_.get_stats(); }
        [[nodiscard]] std::filesystem::path db_path() const { return config_.directory / std::format("partition_{}", partition_id_); }
        [[nodiscard]] uint64_t partition_id() const { return partition_id_; }
        [[nodiscard]] uint64_t active_file_id() const { return active_file_->file_id(); }
    };
}  // namespace bitcask

#endif  // KIO_BITCASK_PARTITION_H
