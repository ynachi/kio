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
    // manual offset tracking. Current size if the writing offset
    // because we use fallocate, which conflicts with O_APPEND
    const auto entry_offset = size_;

    // The file must be opened without O_APPEND for this pwrite-based call to work correctly.
    KIO_TRY(co_await io_worker_.AsyncWriteExactAt(handle_.Get(), entry.GetPayload(), entry_offset));

    if (config_.sync_on_write)
    {
        // After the writing completes, force it to disk if the user configured it
        KIO_TRY(co_await io_worker_.AsyncFdatasync(handle_.Get()));
    }

    // Atomically update size_ for the next writing
    size_ += entry.Size();

    co_return entry_offset;
}

Task<Result<uint64_t>> DataFile::AsyncWrite(std::string_view key, std::span<const char> value, const uint64_t timestamp,
                                            const uint8_t flag)
{
    const auto entry_offset = size_;

    // 1. Prepare Header (21 bytes)
    // Layout: [CRC(4)][Timestamp(8)][Flag(1)][KeyLen(4)][ValueLen(4)]

    char header_buf[kEntryFixedHeaderSize];

    // Fill Metadata
    const auto key_len = static_cast<uint32_t>(key.size());
    const auto val_len = static_cast<uint32_t>(value.size());

    // Calculate CRC
    uint32_t const crc = ComputeCrc32c(timestamp, flag, key_len, val_len, key, value);

    // Write Header
    WriteLe(header_buf, crc);
    WriteLe(header_buf + 4, timestamp);
    WriteLe(header_buf + 12, flag);
    WriteLe(header_buf + 13, key_len);
    WriteLe(header_buf + 17, val_len);

    // 2. Prepare IO Vectors
    iovec iov[3];
    iov[0].iov_base = header_buf;
    iov[0].iov_len = kEntryFixedHeaderSize;
    iov[1].iov_base = const_cast<char*>(key.data());
    iov[1].iov_len = key.size();

    iov[2].iov_base = const_cast<char*>(value.data());
    iov[2].iov_len = value.size();

    // 3. Write using Scatter-Gather
    // We pass offset explicitly. AsyncWritev in worker takes iovec* and count.
    const size_t expected_bytes = kEntryFixedHeaderSize + key.size() + value.size();

    if (const int bytes_written = KIO_TRY(co_await io_worker_.AsyncWritev(handle_.Get(), iov, 3, entry_offset));
        std::cmp_not_equal(bytes_written, expected_bytes))
    {
        // Partial write detected. This usually implies Disk Full or similar issues for local files.
        // Resuming a writev is complex (requires shifting iovecs).
        co_return std::unexpected(Error{ErrorCategory::kFile, EIO});
    }

    if (config_.sync_on_write)
    {
        KIO_TRY(co_await io_worker_.AsyncFdatasync(handle_.Get()));
    }

    size_ += expected_bytes;

    co_return entry_offset;
}

bool DataFile::ShouldRotate(const size_t max_file_size) const
{
    return size_ >= max_file_size;
}

}  // namespace bitcask
