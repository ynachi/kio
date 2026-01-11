//
// Created by Yao ACHI on 10/11/2025.
//

#include "bitcask/include/data_file.h"

#include "bitcask/include/entry.h"
#include "crc32c/crc32c.h"

#include <utility>

using namespace kio;
namespace bitcask
{
namespace
{
// Helper to compute CRC32C over multiple spans using Extend
uint32_t ComputeCrc32c(const uint64_t timestamp, const uint8_t flag, const uint32_t key_len, const uint32_t val_len,
                       const std::string_view key, std::span<const char> value)
{
    // Serialize the metadata part used for CRC calculation (Timestamp..Value)
    // Note: DataEntry layout for CRC is [Timestamp(8)][Flag(1)][KeyLen(4)][ValueLen(4)][Key][Value]
    // The CRC field itself is at the start and excluded.

    // We construct a small buffer for the metadata part
    char meta_buf[17];  // 8 + 1 + 4 + 4
    WriteLe(meta_buf, timestamp);
    WriteLe(meta_buf + 8, flag);
    WriteLe(meta_buf + 9, key_len);
    WriteLe(meta_buf + 13, val_len);

    // Calculate CRC incrementally to avoid allocating a large buffer
    uint32_t crc = crc32c::Crc32c(meta_buf, sizeof(meta_buf));
    crc = crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(key.data()), key.size());
    crc = crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(value.data()), value.size());

    return crc;
}
}  // namespace

DataFile::DataFile(const int fd, const uint64_t file_id, io::Worker& io_worker, BitcaskConfig& config)
    : file_id_(file_id), handle_(fd), io_worker_(io_worker), config_(config)
{
    if (fd < 0)
    {
        // this is a bug from the developer, so just throw
        throw std::invalid_argument("fd cannot be negative");
    }

    // FIX: Validate that O_APPEND is NOT set.
    // O_APPEND causes pwrite() to ignore the offset and always write at EOF,
    // which would break our manual offset tracking and cause data corruption.
    int flags = ::fcntl(fd, F_GETFL);
    if (flags == -1)
    {
        throw std::system_error(errno, std::system_category(), "fcntl(F_GETFL) failed");
    }
    if (flags & O_APPEND)
    {
        throw std::invalid_argument(
            "DataFile cannot be opened with O_APPEND - it breaks pwrite offset semantics. "
            "Remove O_APPEND from write_flags in config.");
    }
}

Task<Result<void>> DataFile::AsyncClose()
{
    if (handle_.IsValid())
    {
        // We can use async_close from worker if we release ownership from a handle first
        // to avoid double close or just rely on handle destructor (sync close).
        // Given the requirement for async_close, we release the FD.
        const int fd = handle_.Release();
        KIO_TRY(co_await io_worker_.AsyncClose(fd));
    }
    co_return {};
}

Task<Result<uint64_t>> DataFile::AsyncWrite(const DataEntry& entry)
{
    const size_t entry_size = entry.Size();

    // Reserve space BEFORE yielding to prevent race condition.
    // Even though we're single-threaded, co_await yields to other coroutines.
    // Without this fix:
    //   1. Coroutine A reads size_ = 100
    //   2. Coroutine A yields on co_await
    //   3. Coroutine B reads size_ = 100 (still!)
    //   4. Both write to offset 100 = DATA CORRUPTION
    const uint64_t entry_offset = size_;
    size_ += entry_size;  // Reserve immediately, BEFORE any co_await

    // Now perform the write - other coroutines will see updated size_
    auto write_result = co_await io_worker_.AsyncWriteExactAt(handle_.Get(), entry.GetPayload(), entry_offset);

    if (!write_result.has_value())
    {
        // Write failed - we have a "hole" in the file.
        // Options:
        // 1. Leave it - recovery will skip corrupt entries via CRC check
        // 2. Try to rollback size_ - but other writes may have happened
        // 3. Mark file as corrupt and force rotation
        //
        // For Bitcask, option 1 is standard - recovery handles holes.
        // The reserved space becomes garbage that recovery skips.
        ALOG_ERROR("Write failed at offset {}, entry_size {}. File may have hole.", entry_offset, entry_size);
        co_return std::unexpected(write_result.error());
    }

    if (config_.sync_on_write)
    {
        KIO_TRY(co_await io_worker_.AsyncFdatasync(handle_.Get()));
    }

    co_return entry_offset;
}

Task<Result<uint64_t>> DataFile::AsyncWrite(std::string_view key, std::span<const char> value, const uint64_t timestamp,
                                            const uint8_t flag)
{
    // Calculate entry size FIRST
    const auto key_len = static_cast<uint32_t>(key.size());
    const auto val_len = static_cast<uint32_t>(value.size());
    const size_t entry_size = kEntryFixedHeaderSize + key_len + val_len;

    const uint64_t entry_offset = size_;
    size_ += entry_size;

    // 3. Prepare Header (21 bytes)
    // Layout: [CRC(4)][Timestamp(8)][Flag(1)][KeyLen(4)][ValueLen(4)]
    char header_buf[kEntryFixedHeaderSize];

    // Calculate CRC
    uint32_t const crc = ComputeCrc32c(timestamp, flag, key_len, val_len, key, value);

    // Write Header
    WriteLe(header_buf, crc);
    WriteLe(header_buf + 4, timestamp);
    WriteLe(header_buf + 12, flag);
    WriteLe(header_buf + 13, key_len);
    WriteLe(header_buf + 17, val_len);

    // 4. Prepare IO Vectors
    iovec iov[3];
    iov[0].iov_base = header_buf;
    iov[0].iov_len = kEntryFixedHeaderSize;
    iov[1].iov_base = const_cast<char*>(key.data());
    iov[1].iov_len = key.size();
    iov[2].iov_base = const_cast<char*>(value.data());
    iov[2].iov_len = value.size();

    // 5. Perform the write - NOW it's safe to yield
    // Other coroutines will see size_ already incremented
    auto write_result = co_await io_worker_.AsyncWritev(handle_.Get(), iov, 3, entry_offset);

    if (!write_result.has_value())
    {
        // Write failed after we reserved space - we have a hole.
        // Recovery will skip this via CRC check. Log for debugging.
        ALOG_ERROR("AsyncWritev failed at offset {}, size {}. Hole in file {}.",
                   entry_offset, entry_size, file_id_);
        co_return std::unexpected(write_result.error());
    }

    const int bytes_written = write_result.value();
    if (std::cmp_not_equal(bytes_written, entry_size))
    {
        // Partial write - also creates a hole/corrupt entry
        ALOG_ERROR("Partial write: expected {} bytes, wrote {} at offset {}",
                   entry_size, bytes_written, entry_offset);
        co_return std::unexpected(Error{ErrorCategory::kFile, EIO});
    }

    if (config_.sync_on_write)
    {
        KIO_TRY(co_await io_worker_.AsyncFdatasync(handle_.Get()));
    }

    co_return entry_offset;
}

bool DataFile::ShouldRotate(const size_t max_file_size) const
{
    return size_ >= max_file_size;
}

}  // namespace bitcask