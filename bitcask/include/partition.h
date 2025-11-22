//
// Created by Yao ACHI on 21/11/2025.
//

#ifndef KIO_BITCASK_PARTITION_H
#define KIO_BITCASK_PARTITION_H
#include "compactor.h"
#include "core/include/sync/baton.h"
#include "data_file.h"
#include "entry.h"

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
        // a single worker for everything. A worker is single-threaded, so no synchronization needed.
        kio::io::Worker& worker_;
        std::unique_ptr<DataFile> active_file_;
        // a cache of opened read-only files
        std::unordered_map<uint64_t, FileHandle> sealed_files_;
        BitcaskConfig config_;
        size_t partition_id_;
        PartitionStats stats_;

        // Startup & Recovery Logic
        kio::Task<kio::Result<void>> recover_file(uint64_t file_id);
        kio::Task<kio::Result<void>> recover_from_data_file(uint64_t file_id, int fd, uint64_t file_size);
        kio::Task<kio::Result<DataEntry>> async_read_entry(int fd, uint64_t offset, uint32_t size) const;

        // File Management
        kio::Task<kio::Result<void>> rotate_active_file();
        kio::Task<kio::Result<std::unique_ptr<DataFile>>> new_data_file(uint64_t id);
        /**
         * @brief Gets the FD that an entry location points to
         * @param it An iterator representing a location in a KeyDir
         * @return the FD to which to location points.
         */
        [[nodiscard]] kio::Result<int> find_fd(SimpleKeydir::const_iterator it);
        static uint64_t generate_file_id() { return get_current_timestamp<std::chrono::seconds>(); }

        // ========================================================================
        // Compaction
        // ========================================================================
        Compactor compactor_;
        kio::DetachedTask compaction_loop();
        void signal_compaction(uint64_t file_id);
        bool should_compact_file(uint64_t file_id) const;
        // Event-driven compaction trigger
        kio::sync::AsyncBaton compaction_trigger_;
        // Find all files which needs compaction
        std::vector<uint64_t> find_fragmented_files() const;

    public:
        // ========================================================================
        // Constructors/Destructors
        // ========================================================================
        Partition(const BitcaskConfig& config, kio::io::Worker& worker, size_t partition_id);
        ~Partition() = default;

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
        kio::Task<kio::Result<void>> sync();
        // force manual compact, scans all the old file and compact if possible
        kio::Task<kio::Result<void>> compact();
        // add method to get stats
    };
}  // namespace bitcask

#endif  // KIO_BITCASK_PARTITION_H
