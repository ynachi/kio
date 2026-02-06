#pragma once
#include "kio/kio.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <list>
#include <string>

#include "bitcask/common.hpp"
#include "bitcask/entry.hpp"

namespace bitcask
{
//========================================
// FD Cache
//========================================
/**
 * @brief LRU cache for file descriptors
 *
 * Prevents FD exhaustion by:
 * 1. Limiting max open files (e.g., 100)
 * 2. Automatically evicting least recently used
 * 3. Reopening files on-demand
 */
class FDCache
{
public:
    explicit FDCache(size_t max_files = 100) : max_open_files_(max_files) {}

    /**
     * @brief Get FD for file_id, opening if not cached
     */
    kio::Task<kio::Result<int>> GetOrOpen(kio::IoContext& ctx, uint64_t file_id, const std::filesystem::path& path);
    /**
     * @brief Remove file from cache (e.g., after compaction)
     */
    kio::Task<> Remove(kio::IoContext& ctx, uint64_t file_id);
    /**
     * @brief Clear all cached FDs
     */
    void Clear()
    {
        cache_.clear();
        lru_list_.clear();
    }
    // Stats
    struct Stats
    {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;

        [[nodiscard]] double HitRate() const
        {
            return hits + misses > 0 ? static_cast<double>(hits) / static_cast<double>(hits + misses) : 0.0;
        }
    };

    [[nodiscard]] const Stats& GetStats() const { return stats_; }
    [[nodiscard]] size_t Size() const { return cache_.size(); }

private:
    struct CacheEntry
    {
        kio::FDGuard handle;
        std::filesystem::path path;
        std::list<uint64_t>::iterator lru_iter;
    };

    size_t max_open_files_;
    std::unordered_map<uint64_t, CacheEntry> cache_;
    std::list<uint64_t> lru_list_;
    Stats stats_;

    /// refresh file id in the cache
    void Touch(uint64_t file_id);
    kio::Task<> EvictOldest(kio::IoContext& ctx);
};

//========================================
// File ID
//========================================
/**
 * @brief File ID format: 64-bit unique identifier
 * * Layout:
 * ┌─────────────┬──────────────┬──────────────┐
 * │ Partition   │  Timestamp   │  Sequence    │
 * │  (16 bits)  │  (32 bits)   │  (16 bits)   │
 * └─────────────┴──────────────┴──────────────┘
 * * - Partition: 0-65535 (supports 64K partitions)
 * - Timestamp: Unix seconds (valid until year 2106)
 * - Sequence: 0-65535 (65K files per second per partition)
 */
struct FileID
{
    uint16_t partition;
    uint32_t timestamp_sec;
    uint16_t sequence;

    /**
     * @brief Encode to 64-bit file ID
     */
    [[nodiscard]] uint64_t Encode() const
    {
        return (static_cast<uint64_t>(partition) << 48) | (static_cast<uint64_t>(timestamp_sec) << 16) |
               static_cast<uint64_t>(sequence);
    }

    /**
     * @brief Decode 64-bit file ID
     */
    static FileID Decode(const uint64_t id)
    {
        return FileID{.partition = static_cast<uint16_t>(id >> 48),
                      .timestamp_sec = static_cast<uint32_t>(id >> 16 & 0xFFFFFFFF),
                      .sequence = static_cast<uint16_t>(id & 0xFFFF)};
    }

    /**
     * @brief Human-readable format for debugging
     */
    [[nodiscard]] std::string Debug() const
    {
        return std::format("FileId(partition={}, timestamp={}, seq={})", partition, timestamp_sec, sequence);
    }
};

/**
 * @brief Single-threaded file ID generator.
 * * Optimized for the Share-Nothing architecture.
 * NOT thread-safe. Must be owned by a single Partition/Worker.
 */
class FileIdGenerator
{
public:
    explicit FileIdGenerator(const uint16_t partition_id) : partition_id_(partition_id) {}

    /**
     * @brief Update the internal state to ensure monotonicity after recovery.
     * This is crucial if the system clock has moved backwards since the files were created.
     */
    void UpdateState(const uint32_t max_timestamp, uint16_t max_sequence)
    {
        if (max_timestamp > last_timestamp_)
        {
            last_timestamp_ = max_timestamp;
            sequence_ = max_sequence;
        }
        else if (max_timestamp == last_timestamp_)
        {
            sequence_ = std::max(max_sequence, sequence_);
        }
    }

    /**
     * @brief Generate next file ID
     * Monotonically increasing. Handles clock skew.
     */
    uint64_t Next()
    {
        auto now = GetCurrentTimestampSec();

        if (now > last_timestamp_)
        {
            // New second - reset sequence
            last_timestamp_ = now;
            sequence_ = 0;
        }
        else
        {
            // The same second OR a clock went backwards (skew).
            // Treat clock skew as "same second" to enforce monotonicity.
            now = last_timestamp_;

            if (sequence_ == 0xFFFF)
            {
                // This is a rare edge case: generating >65k files in 1 second.
                ALOG_WARN("FileIdGenerator: sequence overflow for partition {}", partition_id_);
                sequence_ = 0;
            }
            else
            {
                sequence_++;
            }
        }

        return FileID{.partition = partition_id_, .timestamp_sec = now, .sequence = sequence_}.Encode();
    }

    [[nodiscard]] uint32_t CurrentTimestamp() const { return last_timestamp_; }

    [[nodiscard]] uint16_t PartitionId() const { return partition_id_; }

private:
    uint16_t partition_id_;
    uint32_t last_timestamp_{0};
    uint16_t sequence_{0};

    static uint32_t GetCurrentTimestampSec()
    {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }
};

/**
 * @brief Compare file IDs by timestamp (for sorting during recovery)
 */
inline bool FileIdCompareByTime(const uint64_t a, const uint64_t b)
{
    // Since the layout is Partition(16) | Time(32) | Seq(16),
    // direct integer comparison sorts by Partition, THEN Time, THEN Seq.
    // If we want to sort purely by Time (ignoring Partition), we need decoding, but why would we want that ?

    const auto id_a = FileID::Decode(a);
    const auto id_b = FileID::Decode(b);

    if (id_a.timestamp_sec != id_b.timestamp_sec)
    {
        return id_a.timestamp_sec < id_b.timestamp_sec;
    }

    return id_a.sequence < id_b.sequence;
}

//========================================
// Data File
//========================================
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
     * @param config Bitcask configuration
     *
     * @throws std::invalid_argument if fd < 0 or if O_APPEND is set
     */
    DataFile(const int fd, const uint64_t file_id, BitcaskConfig& config) : file_id_(file_id), fd_(fd), config_(config)
    {
    }

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
     * @param ctx io context
     * @param entry The entry to write
     * @return Offset where entry was written, or error
     */
    kio::Task<kio::Result<uint64_t>> AsyncWrite(kio::IoContext& ctx, const DataEntry& entry);

    /**
     * @brief Write an entry using scatter-gather I/O.
     *
     * More efficient than AsyncWrite(DataEntry) as it avoids copying
     * key/value into a contiguous buffer.
     *
     * Coroutine-safe: Reserves space before yielding to prevent races.
     *
     * @param ctx
     * @param key Key data
     * @param value Value data
     * @param timestamp Entry timestamp
     * @param flag Entry flags (e.g., tombstone)
     * @return Offset where entry was written, or error
     */
    kio::Task<kio::Result<uint64_t>> AsyncWrite(kio::IoContext& ctx, std::string_view key,
                                                std::span<const std::byte> value, uint64_t timestamp, uint8_t flag);

    // Getters
    [[nodiscard]] uint64_t FileId() const { return file_id_; }
    [[nodiscard]] int Fd() { return fd_; }
    [[nodiscard]] uint64_t Size() const { return size_; }
    [[nodiscard]] bool ShouldRotate(size_t max_file_size) const { return size_ >= max_file_size; }

private:
    // timestamp_s based id
    // data_1741971205.db
    uint64_t file_id_{0};
    // FD is owned by the cache via a guard, this is a non-owned raw fd, it should not be closed by this class
    int fd_;
    uint64_t size_{0};
    // useful to perform compaction of files older than X
    // on seal, file metadata is also written to disk
    // data_id_.metadata, or we can rely on fstats
    std::chrono::steady_clock::time_point created_at_{std::chrono::steady_clock::now()};
    //
    BitcaskConfig& config_;
};
//========================================
// Hint File
//========================================
class HintFile
{
    // timestamp-based id
    // hint_1741971205.ht
    uint64_t file_id_{0};
    // the file should be opened with an O_APPEND flag
    int fd_;

public:
    HintFile(const int fd, const uint64_t file_id) : file_id_(file_id), fd_(fd) {}

    HintFile(HintFile&& other) noexcept = default;
    // File is not copyable and cannot be assigned
    HintFile(const HintFile&) = delete;
    HintFile& operator=(const HintFile&) = delete;
    HintFile& operator=(HintFile&& other) noexcept = delete;

    ~HintFile() = default;

    [[nodiscard]] uint64_t FileId() const { return file_id_; }
    // Add this getter so tests can access the fd
    [[nodiscard]] int Fd() const { return fd_; }

    // TODO: this method might create too much allocation. Do not use it.
    // [[nodiscard]] kio::Task<kio::Result<void>> AsyncWrite(kio::IoContext& ctx, const HintEntry&& entry) const
    // {
    //     std::vector<std::byte> buf(entry.Size());
    //     (void)entry.SerializeTo(buf);
    //     // this writes at the end because entry files are created with O_APEND
    //     return kio::AsyncWriteExact(ctx, fd_, buf);
    // }
};

}  // namespace bitcask
