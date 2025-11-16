//
// Created by Yao ACHI on 14/11/2025.
//
#include "bitcask/include/compactor.h"

#include <format>
#include <ylt/struct_pack.hpp>

#include "bitcask/include/entry.h"

using namespace kio;
using namespace kio::io;

namespace bitcask
{
    Task<Result<std::pair<int, int>>> Compactor::prep_compaction(uint64_t new_files_id)
    {
        const auto data_fd = KIO_TRY(co_await io_worker_.async_openat(config_.directory / std::format("data_{}.db", new_files_id), config_.write_flags, config_.file_mode));
        KIO_TRY(co_await io_worker_.async_fallocate(data_fd, 0, config_.max_file_size));
        const auto hint_fd = KIO_TRY(co_await io_worker_.async_openat(config_.directory / std::format("hint_{}.ht", new_files_id), config_.write_flags, config_.file_mode));
        DataFile df();
        co_return std::pair{data_fd, hint_fd};
    }

    // Task<Result<void>> Compactor::compact(uint64_t file_id)
    // {
    //     const auto path = config_.directory / std::format("data_{}.db", file_id);
    //     const int fd = KIO_TRY(co_await io_worker_.async_openat(path, config_.read_flags, config_.file_mode));
    //     size_t pos = 0;
    //
    //     std::vector<char> buf;
    //     buf.resize(config_.read_buffer_size);
    //     std::span read_span(buf);
    //     for (;;)
    //     {
    //         auto res = KIO_TRY(co_await io_worker_.async_read_at(fd, read_span, pos));
    //         if (res == 0 && buf.empty())
    //         {
    //             co_return {};
    //         }
    //         pos += res;
    //
    //         auto decode_pos = 0;
    //         for (;;)
    //         {
    //             auto decode_span = read_span.subspan(decode_pos);
    //             auto entry = struct_pack::deserialize<DataEntry>(decode_span);
    //             if (!entry.has_value())
    //             {
    //                 // not enough data to decode entry
    //                 if (entry.error().ec == struct_pack::errc::no_buffer_space)
    //                 {
    //                     break;
    //                 }
    //                 ALOG_ERROR("Failed to deserialize entry: {}", entry.error().message());
    //                 co_return std::unexpected(Error::from_category(IoError::IODeserialization));
    //             }
    //             decode_pos += struct_pack::get_needed_size(entry.value());
    //
    //             process_entry(entry.value());
    //         }
    //     }
}  // namespace bitcask
// namespace bitcask
