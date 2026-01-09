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
 *
 * IMPORTANT: This function only writes live data to the destination file and updates
 * the KeyDir to point to the new locations. The actual deletion of source files
 * happens in Partition::Compact() after CompactFiles() returns successfully.
 */
Task<Result<void>> ProcessBufferEntries(BytesMut& buffer, uint64_t src_id, const uint64_t file_offset_end,
                                        CompactionContext& ctx, DataFile& dst_file, uint64_t& total_reclaimed_bytes,
                                        uint64_t& entries_copied, uint64_t& entries_skipped)
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
        // file_offset_end is the position in the file corresponding to the end of data read into buffer.
        // buffer.Remaining() is the amount of valid data currently in buffer (including this entry).
        const uint64_t entry_start_offset = file_offset_end - buffer.Remaining();

        // Check if this is the current live version of this key
        auto it = ctx.keydir.find(key_view);
        const bool is_live = (it != ctx.keydir.end() && it->second.file_id == src_id &&
                              it->second.offset == entry_start_offset);

        if (is_live)
        {
            // Extract value span
            std::span val_span(span.data() + kEntryFixedHeaderSize + key_len, val_len);

            // Extract timestamp
            uint64_t timestamp{};
            std::memcpy(&timestamp, span.data() + 4, 8);

            // Write to destination file
            auto write_res = co_await dst_file.AsyncWrite(key_view, val_span, timestamp, kFlagNone);
            if (!write_res.has_value())
            {
                co_return std::unexpected(write_res.error());
            }

            const uint64_t new_offset = write_res.value();
            const uint32_t new_entry_size = kEntryFixedHeaderSize + key_len + val_len;

            // Re-verify KeyDir before updating to handle concurrent modifications
            // This is the critical section - we need to ensure we don't overwrite
            // a newer update that happened during compaction
            auto re_check = ctx.keydir.find(key_view);
            if (re_check != ctx.keydir.end() && re_check->second.file_id == src_id &&
                re_check->second.offset == entry_start_offset)
            {
                // Still pointing to the old location - safe to update
                re_check->second = ValueLocation{
                    .file_id = ctx.dst_file_id,
                    .offset = new_offset,
                    .total_size = new_entry_size,
                    .timestamp_ns = timestamp
                };
            }
            // If re_check fails, a concurrent Put updated this key - that's fine,
            // the new location is already correct

            entries_copied++;
        }
        else
        {
            // Dead entry (overwritten or deleted) - skip it
            total_reclaimed_bytes += entry_size;
            entries_skipped++;
        }

        buffer.Advance(entry_size);
    }
    co_return {};
}

/**
 * @brief Handles the reading loop for a single source file.
 */
Task<Result<void>> CompactSingleFile(uint64_t src_id, CompactionContext& ctx, DataFile& dst_file, BytesMut& read_buffer,
                                     uint64_t& total_reclaimed_bytes, uint64_t& total_entries_copied,
                                     uint64_t& total_entries_skipped)
{
    const auto src_path = ctx.config.directory / std::format("partition_{}/data_{}.db", ctx.partition_id, src_id);

    auto open_res = co_await ctx.worker.AsyncOpenAt(src_path, ctx.config.read_flags, ctx.config.file_mode);
    if (!open_res.has_value())
    {
        ALOG_ERROR("Failed to open source file {} for compaction: {}", src_id, open_res.error());
        // Return error instead of silently continuing - partial compaction is dangerous
        co_return std::unexpected(open_res.error());
    }

    int const src_fd = open_res.value();
    FileHandle const src_handle(src_fd);

    uint64_t file_offset = 0;
    uint64_t const file_size = KIO_TRY(GetFileSize(src_fd));

    ALOG_DEBUG("Compacting file {} ({} bytes)", src_id, file_size);

    while (file_offset < file_size)
    {
        read_buffer.Reserve(64 * 1024);
        auto writable = read_buffer.WritableSpan();
        uint64_t const bytes_to_read = std::min(writable.size(), file_size - file_offset);

        auto read_res = co_await ctx.worker.AsyncReadAt(src_fd, writable.subspan(0, bytes_to_read), file_offset);
        if (!read_res.has_value())
        {
            ALOG_ERROR("Read error during compaction of file {}: {}", src_id, read_res.error());
            co_return std::unexpected(read_res.error());
        }

        if (read_res.value() == 0)
        {
            break;
        }

        read_buffer.CommitWrite(read_res.value());
        file_offset += read_res.value();

        // Process loaded data
        uint64_t entries_copied = 0;
        uint64_t entries_skipped = 0;
        KIO_TRY(co_await ProcessBufferEntries(read_buffer, src_id, file_offset, ctx, dst_file, total_reclaimed_bytes,
                                              entries_copied, entries_skipped));

        total_entries_copied += entries_copied;
        total_entries_skipped += entries_skipped;

        if (read_buffer.ShouldCompact())
        {
            read_buffer.Compact();
        }
    }

    ALOG_DEBUG("Finished compacting file {}: {} entries copied, {} entries skipped", src_id, total_entries_copied,
               total_entries_skipped);

    co_return {};
}

}  // namespace

Task<Result<void>> CompactFiles(CompactionContext& ctx)
{
    ALOG_INFO("Starting compaction for partition {}, merging {} files into new file {}", ctx.partition_id,
              ctx.src_file_ids.size(), ctx.dst_file_id);

    auto dst_path = ctx.config.directory / std::format("partition_{}/data_{}.db", ctx.partition_id, ctx.dst_file_id);
    int dst_fd = KIO_TRY(co_await ctx.worker.AsyncOpenAt(dst_path, ctx.config.write_flags, ctx.config.file_mode));

    DataFile dst_file(dst_fd, ctx.dst_file_id, ctx.worker, ctx.config);
    BytesMut read_buffer(kCompactionBufferSize);

    uint64_t total_reclaimed_bytes = 0;
    uint64_t total_entries_copied = 0;
    uint64_t total_entries_skipped = 0;

    for (uint64_t const src_id : ctx.src_file_ids)
    {
        auto result = co_await CompactSingleFile(src_id, ctx, dst_file, read_buffer, total_reclaimed_bytes,
                                                 total_entries_copied, total_entries_skipped);
        if (!result.has_value())
        {
            // Clean up partial destination file on failure
            co_await dst_file.AsyncClose();
            // Note: Partition::Compact() will handle unlinking the partial file
            co_return std::unexpected(result.error());
        }
    }

    // Sync and close destination file
    KIO_TRY(co_await ctx.worker.AsyncFsync(dst_file.Handle().Get()));

    // Truncate to actual size (in case we pre-allocated)
    const uint64_t actual_size = dst_file.Size();
    KIO_TRY(co_await ctx.worker.AsyncFtruncate(dst_file.Handle().Get(), static_cast<off_t>(actual_size)));

    KIO_TRY(co_await dst_file.AsyncClose());

    // Update destination file stats
    ctx.stats.data_files[ctx.dst_file_id] = PartitionStats::FileStats{
        .total_bytes = actual_size,
        .live_bytes = actual_size,  // All data in new file is live
        .live_entries = total_entries_copied
    };

    ALOG_INFO("Compaction finished for partition {}. Copied {} entries, skipped {} dead entries. "
              "Reclaimed {} bytes. New file size: {} bytes",
              ctx.partition_id, total_entries_copied, total_entries_skipped, total_reclaimed_bytes, actual_size);

    // NOTE: Source file deletion is handled by the caller (Partition::Compact)
    // This separation allows the caller to handle failures and cleanup atomically

    co_return {};
}

}  // namespace bitcask::compactor