//
// Created by Yao ACHI on 10/11/2025.
//

#include "bitcask/include/data_file.h"

#include "bitcask/include/entry.h"

using namespace kio;
namespace bitcask
{
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
        const int kFd = handle_.Release();
        KIO_TRY(co_await io_worker_.AsyncClose(kFd));
    }
    co_return {};
}

Task<Result<uint64_t>> DataFile::AsyncWrite(const DataEntry& entry)
{
    // manual offset tracking. Current size if the writing offset
    // because we use fallocate, which conflicts with O_APPEND
    const auto kEntryOffset = size_;

    // The file must be opened without O_APPEND for this pwrite-based call to work correctly.
    KIO_TRY(co_await io_worker_.AsyncWriteExactAt(handle_.Get(), entry.GetPayload(), kEntryOffset));

    if (config_.sync_on_write)
    {
        // After the writing completes, force it to disk if the user configured it
        KIO_TRY(co_await io_worker_.AsyncFdatasync(handle_.Get()));
    }

    // Atomically update size_ for the next writing
    size_ += entry.Size();

    co_return kEntryOffset;
}

bool DataFile::ShouldRotate(const size_t max_file_size) const
{
    return size_ >= max_file_size;
}

}  // namespace bitcask
