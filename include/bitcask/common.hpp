#pragma once
#include <chrono>
#include <filesystem>

#include <fcntl.h>

#include "uring/error.hpp"
#include "uring/logger.hpp"

namespace bitcask
{

using namespace std::literals;

enum class Durability : uint8_t
{
    /// Data handed to OS page cache only. Lost on crash/power loss.
    /// Fastest (~µs latency). Use for caches, ephemeral logs.
    None = 0,

    /// Data buffered in user-space, flushed to OS on buffer-full or timer.
    /// Lost on crash if not yet flushed. Default for throughput workloads.
    Buffered = 1,

    /// fsync() called after each write. Survives crash.
    /// Slowest (~1-10ms extra latency). Use for critical data.
    SyncOnWrite = 2,
};

struct BitcaskConfig
{
    std::filesystem::path directory;

    mode_t file_mode = 0644;
    mode_t dir_mode = 0755;
    int read_flags = O_RDONLY;

    /**
     * @brief Flags for opening data files for writing.
     *
     * WARNING: Do NOT include O_APPEND!
     *
     * DataFile uses pwrite() with explicit offsets for concurrent coroutine safety.
     * On Linux, O_APPEND causes pwrite() to IGNORE the offset and always write at EOF.
     * This breaks our internal offset tracking and causes data corruption.
     *
     * The constructor will throw if O_APPEND is detected.
     *
     * Safe flags: O_CREAT | O_RDWR (default)
     * We need O_RDWR because Partition::Get() may read from the active file
     * to satisfy read-your-writes consistency.
     */
    int write_flags = O_CREAT | O_RDWR | O_EXCL;

    // File rotation
    size_t max_segment_size = 100 * 1024 * 1024;  // 100MB

    // max size for user space buffering
    size_t flush_buffer_size = 256 * 1024;  // 256k
    std::chrono::milliseconds flush_max_delay{1ms};

    // Durability
    Durability durability = {};

    // Compaction
    bool auto_compact = true;
    double fragmentation_threshold = 0.5;  // 50% dead data
    std::chrono::milliseconds compaction_interval_s{120s};

    // Performance
    size_t read_buffer_size = 64 * 1024;  // 64KB
    size_t write_buffer_size = 4096;

    // Limit open FDs per partition
    size_t max_open_sealed_files = 100;

    /**
     * @brief Validate configuration.
     * @throws std::invalid_argument if the configuration is invalid
     */
    URing::Result<> validate() const noexcept
    {
        if ((write_flags & O_APPEND) != 0)
        {
            ALOG_ERROR(
                "BitcaskConfig: write_flags must NOT include O_APPEND. "
                "O_APPEND breaks pwrite() offset semantics and causes data corruption.");
            return URing::error_from_errc(std::errc::invalid_argument);
        }

        if (max_segment_size == 0)
        {
            ALOG_ERROR("BitcaskConfig: max_file_size must be > 0");
            return URing::error_from_errc(std::errc::invalid_argument);
        }
        return {};
    }

    std::filesystem::path get_hint_file_path(uint64_t file_id, uint64_t shard_id) const
    {
        return directory / std::format("partition_{}/hint_{}.ht", shard_id, file_id);
    }

    std::filesystem::path get_data_file_path(uint64_t file_id, uint64_t shard_id) const
    {
        return directory / std::format("partition_{}/data_{}.db", shard_id, file_id);
    }
};

}  // namespace bitcask
