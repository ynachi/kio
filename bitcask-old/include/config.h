//
// Created by Yao ACHI on 10/11/2025.
//

#ifndef KIO_CONFIG_H
#define KIO_CONFIG_H

#include <chrono>
#include <filesystem>

#include "common.h"

namespace bitcask
{
using namespace std::literals;

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
    int write_flags = O_CREAT | O_RDWR;

    // File rotation
    size_t max_file_size = 100 * 1024 * 1024;  // 100MB

    // Durability
    bool sync_on_write = false;
    std::chrono::milliseconds sync_interval{1000ms};

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
    void Validate() const
    {
        if (directory.empty())
        {
            throw std::invalid_argument("BitcaskConfig: directory cannot be empty");
        }

        if ((write_flags & O_APPEND) != 0)
        {
            throw std::invalid_argument(
                "BitcaskConfig: write_flags must NOT include O_APPEND. "
                "O_APPEND breaks pwrite() offset semantics and causes data corruption.");
        }

        if (max_file_size == 0)
        {
            throw std::invalid_argument("BitcaskConfig: max_file_size must be > 0");
        }
    }
};

}  // namespace bitcask

#endif  // KIO_CONFIG_H