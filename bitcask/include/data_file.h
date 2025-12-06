//
// Created by Yao ACHI on 10/11/2025.
//

#ifndef KIO_DATAFILE_H
#define KIO_DATAFILE_H
#include <filesystem>

#include "../../kio/core/coro.h"
#include "../../kio/core/worker.h"
#include "config.h"
#include "entry.h"
#include "file_handle.h"
#include "kio/include/ds/buffer_pool.h"

namespace bitcask
{
    // TODO, use fallocate to create the datafile
    class DataFile
    {
    public:
        // The fd will be created by the worker thread (the caller) and passed to the constructor
        DataFile(int fd, uint64_t file_id, kio::io::Worker& io_worker, BitcaskConfig& config);

        // not copyable
        DataFile(const DataFile&) = delete;
        DataFile& operator=(const DataFile&) = delete;

        // No move assignable either
        DataFile& operator=(DataFile&& other) noexcept = delete;

        DataFile(DataFile&& other) noexcept = default;

        ~DataFile() = default;

        // Write entry and return the offset and size of the total bytes written (as serialized by the serializer lib)
        kio::Task<kio::Result<std::pair<uint64_t, uint32_t>>> async_write(const DataEntry& entry);
        kio::Task<kio::Result<void>> async_close();

        // Getters
        [[nodiscard]] uint64_t file_id() const { return file_id_; }
        [[nodiscard]] FileHandle& handle() { return handle_; }
        [[nodiscard]] uint64_t size() const { return size_; }
        [[nodiscard]] bool should_rotate(size_t max_file_size) const;

    private:
        // timestamp_s based id
        // data_1741971205.db
        uint64_t file_id_{0};
        FileHandle handle_;
        uint64_t size_{0};
        // useful to perform compaction of files older than X
        // on seal, file metadata is also written to disk
        // data_id_.metadata, or we can rely on fstats
        std::chrono::steady_clock::time_point created_at_{std::chrono::steady_clock::now()};
        kio::io::Worker& io_worker_;
        //
        BitcaskConfig& config_;
    };
}  // namespace bitcask

#endif  // KIO_DATAFILE_H
