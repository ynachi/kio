//
// Created by Yao ACHI on 10/11/2025.
//

#ifndef KIO_CONFIG_H
#define KIO_CONFIG_H

#include <chrono>
#include <fcntl.h>
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
        int write_flags = O_CREAT | O_WRONLY | O_APPEND;

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
    };

}  // namespace bitcask

#endif  // KIO_CONFIG_H
