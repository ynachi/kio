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
     * the worker and the configs. It is not meant to be used directly, as a standalone unit. But if for some reasons, you do use it
     * this way, make sure the worker and the configs outlive any bitcask operation. One way to do that is to not exit your main
     * thread until every DB work is done.
     */
    class Partition
    {
        SimpleKeydir keydir_{};
        kio::io::Worker& worker_;
        std::unique_ptr<DataFile> active_file_;
        FDCache fd_cache_;
        FileIdGenerator file_id_gen_;

        BitcaskConfig config_{};
        size_t partition_id_;
        PartitionStats stats_{};

        kio::sync::AsyncBaton compaction_trigger_;
        compactor::CompactionLimits compaction_limits_{};

        // Helper methods
        kio::Task<kio::Result<DataEntry>> async_read_entry(int fd, uint64_t offset, uint32_t size) const;
        kio::Task<kio::Result<void>> rotate_active_file();
        kio::Task<kio::Result<int>> find_fd(uint64_t file_id);

        kio::DetachedTask compaction_loop();
        void signal_compaction(uint64_t file_id);
        bool should_compact_file(uint64_t file_id) const;
        std::vector<uint64_t> find_fragmented_files() const;
        std::filesystem::path get_data_file_path(uint64_t file_id) const;

    public:
        Partition(const BitcaskConfig& config, kio::io::Worker& worker, size_t partition_id);

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
    };
}  // namespace bitcask

#endif  // KIO_BITCASK_PARTITION_H
