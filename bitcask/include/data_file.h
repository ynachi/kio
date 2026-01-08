//
// Created by Yao ACHI on 10/11/2025.
//

#ifndef KIO_DATAFILE_H
#define KIO_DATAFILE_H

#include "config.h"
#include "entry.h"
#include "file_handle.h"
#include "kio/core/coro.h"
#include "kio/core/worker.h"

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

    // Write an entry and return the offset
    kio::Task<kio::Result<uint64_t>> AsyncWrite(const DataEntry& entry);

    // Write an entry, use scatter gater for optimization
    kio::Task<kio::Result<uint64_t>> AsyncWrite(std::string_view key, std::span<const char> value, uint64_t timestamp,
                                                uint8_t flag);

    kio::Task<kio::Result<void>> AsyncClose();

    // Getters
    [[nodiscard]] uint64_t FileId() const { return file_id_; }
    [[nodiscard]] FileHandle& Handle() { return handle_; }
    [[nodiscard]] uint64_t Size() const { return size_; }
    [[nodiscard]] bool ShouldRotate(size_t max_file_size) const;

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