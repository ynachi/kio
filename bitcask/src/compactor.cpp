#include "bitcask/include/compactor.h"

#include "bitcask/include/data_file.h"
#include "bitcask/include/entry.h"
#include "kio/core/async_logger.h"
#include "kio/core/bytes_mut.h"

#include <cstring>
#include <filesystem>

namespace bitcask::compactor
{

using namespace kio;
using namespace kio::io;

static constexpr size_t kCompactionBufferSize = 4 * 1024 * 1024;

namespace
{

/**
 * @brief Processes entries currently available in the buffer.
 * Filters live entries using KeyDir and writes them to the destination file.
 */
Task<Result<void>> ProcessBufferEntries(BytesMut& buffer, uint64_t src_id, const uint64_t file_offset_end,
                                        CompactionContext& ctx, DataFile& dst_file, uint64_t& total_reclaimed_bytes)
{
    while (buffer.Remaining() >= kEntryFixedHeaderSize)
    {
        auto span = buffer.ReadableSpan();

        auto read_u32 = [&](const size_t offset)
        {
            uint32_t val{};
            std::memcpy(&val, span.data() + offset, 4);
            return val;
        };

        uint32_t const key_len = read_u32(13);
        uint32_t const val_len = read_u32(17);
        uint32_t const entry_size = kEntryFixedHeaderSize + key_len + val_len;

        if (buffer.Remaining() < entry_size)
        {
            if (entry_size > buffer.Capacity())
            {
                buffer.Reserve(entry_size - buffer.Capacity() + 1024);
            }
            break;
        }

        std::string_view key_view(span.data() + kEntryFixedHeaderSize, key_len);

        // Calculate the absolute offset of this entry in the source file.
        // file_offset_end is the position in the file corresponding to the end of valid data in buffer.
        // buffer.Remaining() is the amount of valid data currently in buffer (including this entry).
        uint64_t const entry_start_offset = file_offset_end - buffer.Remaining();

        if (auto it = ctx.keydir.find(key_view);
            it != ctx.keydir.end() && it->second.file_id == src_id && it->second.offset == entry_start_offset)
        {
            std::span val_span(span.data() + kEntryFixedHeaderSize + key_len, val_len);

            uint64_t timestamp{};
            std::memcpy(&timestamp, span.data() + 4, 8);

            if (auto write_res = co_await dst_file.AsyncWrite(key_view, val_span, timestamp, kFlagNone);
                write_res.has_value())
            {
                uint64_t const new_offset = write_res.value();

                // Re-verify KeyDir to ensure we don't overwrite a newer update
                if (auto re_check = ctx.keydir.find(key_view); re_check != ctx.keydir.end() &&
                                                               re_check->second.file_id == src_id &&
                                                               re_check->second.offset == entry_start_offset)
                {
                    re_check->second = ValueLocation{ctx.dst_file_id, new_offset, entry_size, timestamp};
                }
            }
            else
            {
                // If write fails, we should probably fail the whole compaction
                co_return std::unexpected(write_res.error());
            }
        }
        else
        {
            total_reclaimed_bytes += entry_size;
        }

        buffer.Advance(entry_size);
    }
    co_return {};
}

/**
 * @brief Handles the reading loop for a single source file.
 */
Task<Result<void>> CompactSingleFile(uint64_t src_id, CompactionContext& ctx, DataFile& dst_file, BytesMut& read_buffer,
                                     uint64_t& total_reclaimed_bytes)
{
    const auto src_path = ctx.config.directory / std::format("partition_{}/data_{}.db", ctx.partition_id, src_id);

    auto open_res = co_await ctx.worker.AsyncOpenat(src_path, ctx.config.read_flags, ctx.config.file_mode);
    if (!open_res.has_value())
    {
        ALOG_ERROR("Failed to open source file {} for compaction: {}", src_id, open_res.error());
        // We skip this file but continue compaction for others, or should we fail?
        // Continuing allows partial compaction, but failing is safer.
        // Let's skip and log for now as per original logic.
        co_return {};
    }

    int const src_fd = open_res.value();
    FileHandle const src_handle(src_fd);

    uint64_t file_offset = 0;
    uint64_t const file_size = KIO_TRY(GetFileSize(src_fd));

    while (file_offset < file_size)
    {
        read_buffer.Reserve(64 * 1024);
        auto writable = read_buffer.WritableSpan();
        uint64_t const bytes_to_read = std::min(writable.size(), file_size - file_offset);

        auto read_res = co_await ctx.worker.AsyncReadAt(src_fd, writable.subspan(0, bytes_to_read), file_offset);
        if (!read_res.has_value() || read_res.value() == 0)
        {
            break;
        }

        read_buffer.CommitWrite(read_res.value());
        file_offset += read_res.value();

        // Process loaded data
        KIO_TRY(co_await ProcessBufferEntries(read_buffer, src_id, file_offset, ctx, dst_file, total_reclaimed_bytes));

        if (read_buffer.ShouldCompact())
        {
            read_buffer.Compact();
        }
    }

    co_return {};
}

}  // namespace

Task<Result<void>> CompactFiles(CompactionContext& ctx)
{
    ALOG_INFO("Starting compaction for partition {}, merging {} files into new file {}", ctx.partition_id,
              ctx.src_file_ids.size(), ctx.dst_file_id);

    auto dst_path = ctx.config.directory / std::format("partition_{}/data_{}.db", ctx.partition_id, ctx.dst_file_id);
    int dst_fd = KIO_TRY(co_await ctx.worker.AsyncOpenat(dst_path, ctx.config.write_flags, ctx.config.file_mode));

    DataFile dst_file(dst_fd, ctx.dst_file_id, ctx.worker, ctx.config);
    BytesMut read_buffer(kCompactionBufferSize);
    uint64_t total_reclaimed_bytes = 0;

    for (uint64_t const src_id : ctx.src_file_ids)
    {
        KIO_TRY(co_await CompactSingleFile(src_id, ctx, dst_file, read_buffer, total_reclaimed_bytes));
    }

    KIO_TRY(co_await dst_file.AsyncClose());
    ALOG_INFO("Compaction finished. Reclaimed {} bytes.", total_reclaimed_bytes);
    co_return {};
}

}  // namespace bitcask::compactor