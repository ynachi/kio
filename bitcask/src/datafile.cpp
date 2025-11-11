//
// Created by Yao ACHI on 10/11/2025.
//

#include "bitcask/include/datafile.h"

#include "bitcask/include/entry.h"

using namespace kio;
namespace bitcask
{
    DataFile::DataFile(const int fd, const uint64_t file_id, io::Worker& io_worker, BufferPool& bp, Config& config) :
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
        KIO_TRY(co_await io_worker_.async_close(file_id_));
        file_id_ = -1;
        fd_ = -1;
        co_return {};
    }

    Task<Result<Entry>> DataFile::async_read(const uint64_t offset, const uint32_t size)
    {
        auto buf = buffer_pool_.acquire(size);
        auto span = buf.span(size);
        KIO_TRY(co_await io_worker_.async_read_exact_at(fd_, span, offset));
        auto [entry, _] = KIO_TRY(Entry::deserialize(span));
        co_return std::move(entry);
    }

}  // namespace bitcask
