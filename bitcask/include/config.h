//
// Created by Yao ACHI on 10/11/2025.
//

#ifndef KIO_CONFIG_H
#define KIO_CONFIG_H

#include <chrono>
#include <fcntl.h>
#include <filesystem>

namespace bitcask
{
    using namespace std::literals;

    struct Config
    {
        std::filesystem::path directory;

        /// file mode
        mode_t file_mode = 0644;
        /// directory mode
        mode_t dir_mode = 0755;
        /// Read open options
        int read_flags = O_RDONLY;
        /// write flags
        int write_flags = O_CREAT | O_WRONLY | O_APPEND;

        // File rotation
        size_t max_file_size = 100 * 1024 * 1024;  // 100MB
        // Durability
        bool sync_on_write = false;
        /// Periodic flush or the written data
        std::chrono::milliseconds sync_interval{1000ms};

        // Compaction
        bool auto_compact = true;
        double fragmentation_threshold = 0.5;  // 50% dead data triggers compaction

        // Performance
        size_t read_buffer_size = 4096;
        size_t write_buffer_size = 4096;
    };

}  // namespace bitcask

#endif  // KIO_CONFIG_H
