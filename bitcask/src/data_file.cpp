//
// Created by Yao ACHI on 10/11/2025.
//

#include "bitcask/include/data_file.h"

#include "bitcask/include/entry.h"

using namespace kio;
namespace bitcask
{
    DataFile::DataFile(const int fd, const uint64_t file_id, io::Worker& io_worker, BufferPool& bp, BitcaskConfig& config) :
        file_id_(file_id), fd_(fd), io_worker_(io_worker), buffer_pool_(bp), config_(config)
    {
        if (fd_ < 0)
        {
            // this is a bug from the developer, so just throw
            throw std::invalid_argument("fd cannot be negative");
        }
    }

    Task<Result<void>> DataFile::async_close()
    {
        KIO_TRY(co_await io_worker_.async_close(fd_));
        fd_ = -1;
        co_return {};
    }

    Task<Result<DataEntry>> DataFile::async_read(const uint64_t offset, const uint32_t size) const
    {
        auto buf = buffer_pool_.acquire(size);
        const auto span = buf.span(size);
        KIO_TRY(co_await io_worker_.async_read_exact_at(fd_, span, offset));
        co_return DataEntry::deserialize(span);
    }

    Task<Result<uint64_t>> DataFile::async_write(const DataEntry& entry)
    {
        auto serialized_entry = entry.serialize();
        // O_APPEND mode: physical positioning handled by kernel
        // writing at the end of the file.
        KIO_TRY(co_await io_worker_.async_write_exact(fd_, serialized_entry));

        if (config_.sync_on_write)
        {
            // After the writing completes, force it to disk.
            // We use fdatasync for better performance.
            KIO_TRY(co_await io_worker_.async_fdatasync(fd_));
        }

        auto entry_offset = size_;
        size_ += serialized_entry.size();
        co_return entry_offset;
    }

    bool DataFile::should_rotate(const size_t max_file_size) const { return size_ >= max_file_size; }


}  // namespace bitcask
