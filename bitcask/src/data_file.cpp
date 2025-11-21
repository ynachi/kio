//
// Created by Yao ACHI on 10/11/2025.
//

#include "bitcask/include/data_file.h"

#include "bitcask/include/entry.h"

using namespace kio;
namespace bitcask
{
    DataFile::DataFile(const int fd, const uint64_t file_id, io::Worker& io_worker, BitcaskConfig& config) : file_id_(file_id), handle_(fd), io_worker_(io_worker), config_(config)
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
            // We can use async_close from worker if we release ownership from handle first
            // to avoid double close, or just rely on handle destructor (sync close).
            // Given the requirement for async_close, we release the FD.
            const int fd = handle_.release();
            KIO_TRY(co_await io_worker_.async_close(fd));
        }
        co_return {};
    }

    Task<Result<uint64_t>> DataFile::async_write(const DataEntry& entry)
    {
        auto serialized_entry = entry.serialize();
        // O_APPEND mode: physical positioning handled by kernel
        // writing at the end of the file.
        KIO_TRY(co_await io_worker_.async_write_exact(handle_.get(), serialized_entry));

        if (config_.sync_on_write)
        {
            // After the writing completes, force it to disk.
            // We use fdatasync for better performance.
            KIO_TRY(co_await io_worker_.async_fdatasync(handle_.get()));
        }

        auto entry_offset = size_;
        size_ += serialized_entry.size();
        co_return entry_offset;
    }

    bool DataFile::should_rotate(const size_t max_file_size) const { return size_ >= max_file_size; }


}  // namespace bitcask
