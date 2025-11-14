//
// Created by Yao ACHI on 14/11/2025.
//

#ifndef KIO_HINT_FILE_H
#define KIO_HINT_FILE_H
#include <filesystem>
#include <string>

#include "config.h"
#include "core/include/ds/buffer_pool.h"
#include "core/include/io/worker.h"
#include "entry.h"
#include "keydir.h"

namespace bitcask
{
    class HintFile
    {
        // timestamp-based id
        // hint_1741971205.hint
        uint64_t file_id_{0};
        // the file should be opened with an O_APPEND flag
        int fd_{-1};
        kio::io::Worker& io_worker_;
        kio::BufferPool& buffer_pool_;

        BitcaskConfig& config_;


    public:
        HintFile(int fd, uint64_t file_id, kio::io::Worker& io_worker, kio::BufferPool& bp, BitcaskConfig& config);

        HintFile(HintFile&& other) noexcept : file_id_(other.file_id_), fd_(other.fd_), io_worker_(other.io_worker_), buffer_pool_(other.buffer_pool_), config_(other.config_) { other.fd_ = -1; }

        // File is not copyable and cannot be assigned
        HintFile(const HintFile&) = delete;
        HintFile& operator=(const HintFile&) = delete;
        HintFile& operator=(HintFile&& other) noexcept = delete;

        ~HintFile()
        {
            if (fd_ >= 0)
            {
                ALOG_WARN("Hint file {} is being destroyed without being closed", file_id_);
                close(fd_);
            };
            // avoid double close
            fd_ = -1;
        }

        [[nodiscard]]
        uint64_t file_id() const
        {
            return file_id_;
        }

        // Add this getter so tests can access the fd
        [[nodiscard]]
        int fd() const
        {
            return fd_;
        }

        kio::Task<kio::Result<void>> async_write(const HintEntry&& entry) const;  // NOLINT on [[no_discard]]

        // Read hint file and populate KeyDir
        kio::Task<kio::Result<void>> async_read(KeyDir& keydir) const;
    };

}  // namespace bitcask

#endif  // KIO_HINT_FILE_H
