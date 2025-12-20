//
// Created by Yao ACHI on 10/11/2025.
//

#include "bitcask/include/data_file.h"

#include "bitcask/include/entry.h"

using namespace kio;
namespace bitcask
{
DataFile::DataFile(const int fd, const uint64_t file_id, io::Worker& io_worker, BitcaskConfig& config) :
    file_id_(file_id), handle_(fd), io_worker_(io_worker), config_(config)
{
    if (fd < 0)
    {
        // this is a bug from the developer, so just throw
        throw std::invalid_argument("fd cannot be negative");
    }
}

Task<Result<void>> DataFile::async_close()
{
    if (handle_.is_valid())
    {
        // We can use async_close from worker if we release ownership from a handle first
        // to avoid double close or just rely on handle destructor (sync close).
        // Given the requirement for async_close, we release the FD.
        const int fd = handle_.release();
        KIO_TRY(co_await io_worker_.async_close(fd));
    }
    co_return {};
}

Task<Result<std::pair<uint64_t, uint32_t>>> DataFile::async_write(const DataEntry& entry)
{
    auto serialized_entry = entry.serialize();

    // manual offset tracking. Current size if the writing offset
    // because we use fallocate which conflicts with O_APPEND
    const auto entry_offset = size_;
    const auto written_len = static_cast<uint32_t>(serialized_entry.size());

    // The file must be opened without O_APPEND for this pwrite-based call to work correctly.
    KIO_TRY(co_await io_worker_.async_write_exact_at(handle_.get(), serialized_entry, entry_offset));

    if (config_.sync_on_write)
    {
        // After the writing completes, force it to disk if the user configured it
        KIO_TRY(co_await io_worker_.async_fdatasync(handle_.get()));
    }

    // Atomically update size_ for the next write
    size_ += written_len;

    co_return std::pair{entry_offset, written_len};
}

bool DataFile::should_rotate(const size_t max_file_size) const
{
    return size_ >= max_file_size;
}

}  // namespace bitcask
