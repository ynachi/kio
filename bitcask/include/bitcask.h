//
// Created by Yao ACHI on 20/11/2025.
//

#ifndef KIO_BITCASK_H
#define KIO_BITCASK_H
#include <memory>

#include "config.h"
#include "core/include/io/worker_pool.h"
#include "data_file.h"
#include "keydir.h"

namespace bitcask
{
    class BitKV
    {
    public:
        // Factory method: Creates and initializes the DB (including recovery)
        static kio::Task<kio::Result<std::unique_ptr<BitKV>>> open(BitcaskConfig config, kio::io::IOPool& io_pool);

        // Core Operations
        kio::Task<kio::Result<void>> put(std::string key, std::string value);
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

        // Lifecycle
        kio::Task<kio::Result<void>> close();
        ~BitKV();

    private:
        // Private constructor (use open() factory)
        BitKV(BitcaskConfig config, kio::io::Worker& worker);

        // Startup & Recovery Logic
        kio::Task<kio::Result<void>> load_files();
        kio::Task<kio::Result<void>> recover_file(uint64_t file_id);
        kio::Task<kio::Result<void>> recover_from_data_file(uint64_t file_id, int fd, uint64_t file_size);

        // File Management
        kio::Task<kio::Result<void>> rotate_active_file();
        kio::Task<kio::Result<std::unique_ptr<DataFile>>> new_data_file(uint64_t id);

        BitcaskConfig config_;
        kio::io::IOPool& io_pool_;
        KeyDir keydir_;

        // The single mutable file for writes
        std::unique_ptr<DataFile> active_file_;
        uint64_t active_file_id_{0};

        // Immutable files for reads
        std::unordered_map<uint64_t, std::unique_ptr<DataFile>> older_files_;
    };

}  // namespace bitcask

#endif  // KIO_BITCASK_H
