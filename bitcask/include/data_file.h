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

#include <fcntl.h>  // For O_APPEND check in constructor

namespace bitcask
{

/**
 * @brief Manages a single append-only data file for Bitcask storage.
 *
 * Thread Safety:
 * - DataFile is designed for single-worker access (share-nothing architecture)
 * - However, multiple COROUTINES on the same worker can access it concurrently
 * - The AsyncWrite methods handle this by reserving space BEFORE yielding
 *
 * IMPORTANT: Do NOT open the file with O_APPEND flag!
 * O_APPEND causes pwrite() to ignore the offset parameter and always write at EOF.
 * This breaks our manual offset tracking and causes data corruption.
 */
class DataFile
{
public:
    /**
     * @brief Construct a DataFile wrapper.
     *
     * @param fd File descriptor (must be open for writing, WITHOUT O_APPEND)
     * @param file_id Unique identifier for this file
     * @param io_worker Worker that owns this file (for async I/O)
     * @param config Bitcask configuration
     *
     * @throws std::invalid_argument if fd < 0 or if O_APPEND is set
     */
    DataFile(int fd, uint64_t file_id, kio::io::Worker& io_worker, BitcaskConfig& config);

    // not copyable
    DataFile(const DataFile&) = delete;
    DataFile& operator=(const DataFile&) = delete;

    // No move assignable either
    DataFile& operator=(DataFile&& other) noexcept = delete;

    DataFile(DataFile&& other) noexcept = default;

    ~DataFile() = default;

    /**
     * @brief Write a pre-constructed entry to the file.
     *
     * Coroutine-safe: Reserves space before yielding to prevent races.
     *
     * @param entry The entry to write
     * @return Offset where entry was written, or error
     */
    kio::Task<kio::Result<uint64_t>> AsyncWrite(const DataEntry& entry);

    /**
     * @brief Write an entry using scatter-gather I/O.
     *
     * More efficient than AsyncWrite(DataEntry) as it avoids copying
     * key/value into a contiguous buffer.
     *
     * Coroutine-safe: Reserves space before yielding to prevent races.
     *
     * @param key Key data
     * @param value Value data
     * @param timestamp Entry timestamp
     * @param flag Entry flags (e.g., tombstone)
     * @return Offset where entry was written, or error
     */
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