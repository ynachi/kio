//
// Created by Yao ACHI on 14/11/2025.
//

#include "bitcask/include/hint_file.h"
using namespace kio;
using namespace kio::io;

namespace bitcask
{
    HintFile::HintFile(const int fd, const uint64_t file_id, Worker& io_worker, BitcaskConfig& config) : file_id_(file_id), handle_(fd), io_worker_(io_worker), config_(config)
    {
        if (fd < 0)
        {
            // this is a bug from the developer, so just throw
            throw std::invalid_argument("fd cannot be negative");
        }
    }

    Task<Result<void>> HintFile::async_write(const HintEntry&& entry) const
    {
        auto data = struct_pack::serialize(entry);
        KIO_TRY(co_await io_worker_.async_write_exact(handle_.get(), data));
        co_return {};
    }

    Task<Result<void>> HintFile::async_read(KeyDir& keydir) const
    {
        const auto buf = KIO_TRY(co_await read_file_content(io_worker_, handle_.get()));
        if (buf.empty())
        {
            co_return {};
        }

        std::span buf_span(buf);

        while (!buf_span.empty())
        {
            const auto res = struct_pack::deserialize<HintEntry>(buf_span);
            if (!res.has_value())
            {
                ALOG_ERROR("Failed to deserialize entry: {}", res.error().message());
                co_return std::unexpected(Error{ErrorCategory::Serialization, kIoDeserialization});
            }

            const auto& entry = res.value();
            const size_t bytes_this_entry = struct_pack::get_needed_size(entry);

            keydir.put_if_newer(std::string(entry.key), {file_id_, entry.entry_pos, entry.total_sz, entry.timestamp_ns});
            buf_span = buf_span.subspan(bytes_this_entry);
        }

        co_return {};
    }

}  // namespace bitcask
