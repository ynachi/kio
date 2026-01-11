//
// Created by Yao ACHI on 14/11/2025.
//

#ifndef KIO_HINT_FILE_H
#define KIO_HINT_FILE_H

#include "config.h"
#include "entry.h"
#include "file_handle.h"
#include "kio/core/worker.h"

namespace bitcask
{
class HintFile
{
    // timestamp-based id
    // hint_1741971205.ht
    uint64_t file_id_{0};
    // the file should be opened with an O_APPEND flag
    FileHandle handle_;
    kio::io::Worker& io_worker_;

    BitcaskConfig& config_;

public:
    HintFile(int fd, uint64_t file_id, kio::io::Worker& io_worker, BitcaskConfig& config);

    HintFile(HintFile&& other) noexcept = default;
    // File is not copyable and cannot be assigned
    HintFile(const HintFile&) = delete;
    HintFile& operator=(const HintFile&) = delete;
    HintFile& operator=(HintFile&& other) noexcept = delete;

    ~HintFile() = default;

    [[nodiscard]] uint64_t FileId() const { return file_id_; }
    // Add this getter so tests can access the fd
    [[nodiscard]] int Fd() const { return handle_.Get(); }

    [[nodiscard]] kio::Task<kio::Result<void>> AsyncWrite(const HintEntry&& entry) const
    {
        return io_worker_.AsyncWriteExact(handle_.Get(), entry.Serialize());
    }
};

}  // namespace bitcask

#endif  // KIO_HINT_FILE_H
