//
// Created by Yao ACHI on 10/11/2025.
//

#ifndef KIO_DATAFILE_H
#define KIO_DATAFILE_H
#include <filesystem>

#include "config.h"
#include "core/include/coro.h"
#include "core/include/ds/buffer_pool.h"
#include "core/include/io/worker.h"
#include "entry.h"

namespace bitcask
{
    // TODO, use fallocate to create the datafile
    class DataFile
    {
    public:
        // The fd will be created by the worker thread (the caller) and passed to the constructor
        DataFile(int fd, uint64_t file_id, kio::io::Worker& io_worker, kio::BufferPool& bp, Config& config);

        // File is not copyable and cannot be assigned
        DataFile(const DataFile&) = delete;
        DataFile& operator=(const DataFile&) = delete;
        DataFile& operator=(DataFile&& other) noexcept = delete;

        DataFile(DataFile&& other) noexcept : file_id_(other.file_id_), fd_(other.fd_), io_worker_(other.io_worker_), buffer_pool_(other.buffer_pool_), config_(other.config_) { other.fd_ = -1; }

        ~DataFile()
        {
            if (fd_ >= 0)
            {
                ALOG_WARN("DataFile {} is being destroyed without being closed", path_.string());
                close(fd_);
            };
            // avoid double close
            fd_ = -1;
        }

        // Write entry and return offset
        kio::Task<kio::Result<uint64_t>> async_write(const DataEntry& entry);

        // Read the entry at offset. size is total entry size as it is known in advance
        [[nodiscard]]
        kio::Task<kio::Result<DataEntry>> async_read(uint64_t offset, uint32_t size) const;

        // Close file
        kio::Task<kio::Result<void>> async_close();

        // Getters
        [[nodiscard]]
        uint32_t file_id() const
        {
            return file_id_;
        }
        [[nodiscard]]
        uint64_t size() const
        {
            return size_;
        }
        [[nodiscard]]
        const std::filesystem::path& path() const
        {
            return path_;
        }

        // Check if a file should be rotated
        [[nodiscard]]
        bool should_rotate(size_t max_file_size) const;

    private:
        std::filesystem::path path_;
        // timestamp based id
        uint64_t file_id_{0};
        int fd_{-1};
        uint64_t size_{0};
        kio::io::Worker& io_worker_;
        kio::BufferPool& buffer_pool_;
        //
        Config& config_;
    };
}  // namespace bitcask

#endif  // KIO_DATAFILE_H
