//
// Created by Yao ACHI on 21/11/2025.
//

#include "bitcask/include/compactor.h"

#include "bitcask/include/file_handle.h"

#include <format>

#include <ylt/struct_pack.hpp>

using namespace kio;
using namespace kio::io;

namespace bitcask::compactor
{
Task<Result<void>> CompactFiles(CompactionContext& ctx)
{
    ALOG_INFO("Starting N-to-1 compaction: {} files -> file {}", ctx.src_file_ids.size(), ctx.dst_file_id);

    // Initialize destination file stats
    ctx.stats.data_files[ctx.dst_file_id] = PartitionStats::FileStats{};

    // Create destination files
    const auto data_path = detail::GetDataFilePath(ctx, ctx.dst_file_id);
    const auto hint_path = detail::GetHintFilePath(ctx, ctx.dst_file_id);

    const int data_fd =
        KIO_TRY(co_await ctx.io_worker.AsyncOpenat(data_path, ctx.config.write_flags, ctx.config.file_mode));
    const int hint_fd =
        KIO_TRY(co_await ctx.io_worker.AsyncOpenat(hint_path, ctx.config.write_flags, ctx.config.file_mode));

    FileHandle const data_handle(data_fd);
    FileHandle const hint_handle(hint_fd);

    // Pre-allocate
    KIO_TRY(co_await ctx.io_worker.AsyncFallocate(data_fd, 0, static_cast<off_t>(ctx.config.max_file_size)));

    // Compact each source file
    for (uint64_t src_file_id : ctx.src_file_ids)
    {
        // log error regarding one file failing
        if (auto result = co_await CompactOneFile(ctx, src_file_id, data_fd, hint_fd); !result.has_value())
        {
            ALOG_ERROR("Failed to compact file {}: {}", src_file_id, result.error());
            co_return std::unexpected(result.error());
        }
    }

    // Final flush
    KIO_TRY(co_await detail::CommitBatch(ctx, data_fd, hint_fd));

    // Sync
    KIO_TRY(co_await ctx.io_worker.AsyncFsync(data_fd));
    KIO_TRY(co_await ctx.io_worker.AsyncFsync(hint_fd));

    double reduction =
        ctx.bytes_read > 0
            ? 100.0 * (1.0 - static_cast<double>(ctx.bytes_written) / static_cast<double>(ctx.bytes_read))
            : 0.0;

    ALOG_INFO("Compaction complete: kept {}, discarded {}, wrote {}KB (from {}KB, {:.1f}% reduction)", ctx.entries_kept,
              ctx.entries_discarded, ctx.bytes_written / 1024, ctx.bytes_read / 1024, reduction);

    co_return {};
}

Task<Result<void>> CompactOneFile(CompactionContext& ctx, const uint64_t src_file_id, const int dst_data_fd,
                                  const int dst_hint_fd)
{
    detail::ResetBatches(ctx);
    ctx.decode_buffer.Clear();

    auto [src_fd, file_size] = KIO_TRY(co_await detail::OpenSourceFile(ctx, src_file_id));

    FileHandle const src_handle(src_fd);
    ctx.bytes_read += file_size;

    KIO_TRY(co_await detail::StreamAndCompactFile(ctx, src_fd, dst_data_fd, dst_hint_fd, src_file_id));

    KIO_TRY(co_await detail::CommitBatch(ctx, dst_data_fd, dst_hint_fd));

    // Update stats: a source file is gone
    if (const auto it = ctx.stats.data_files.find(src_file_id); it != ctx.stats.data_files.end())
    {
        ctx.stats.bytes_reclaimed_total += it->second.total_bytes - it->second.live_bytes;
        ctx.stats.data_files.erase(it);
    }

    ctx.stats.files_compacted_total++;

    KIO_TRY(co_await detail::CleanupSourceFiles(ctx, src_file_id));

    co_return {};
}

namespace detail
{
Task<Result<std::pair<int, uint64_t>>> OpenSourceFile(const CompactionContext& ctx, const uint64_t src_file_id)
{
    const auto src_path = GetDataFilePath(ctx, src_file_id);
    const auto src_fd =
        KIO_TRY(co_await ctx.io_worker.AsyncOpenat(src_path, ctx.config.read_flags, ctx.config.file_mode));

    struct stat st{};
    uint64_t file_size = 0;
    if (::fstat(src_fd, &st) == 0)
    {
        file_size = st.st_size;
    }

    co_return std::pair{src_fd, file_size};
}

Task<Result<void>> StreamAndCompactFile(CompactionContext& ctx, const int src_fd, const int dst_data_fd,
                                        const int dst_hint_fd, uint64_t src_file_id)
{
    uint64_t file_read_pos = 0;

    for (;;)
    {
        // Reserve space in the buffer
        ctx.decode_buffer.Reserve(ctx.config.read_buffer_size);
        auto write_span = ctx.decode_buffer.WritableSpan();

        // Read from upstream IO
        const auto bytes_read = KIO_TRY(co_await ctx.io_worker.AsyncReadAt(src_fd, write_span, file_read_pos));

        ctx.decode_buffer.CommitWrite(bytes_read);

        if (bytes_read == 0)
        {
            if (!ctx.decode_buffer.IsEmpty())
            {
                // Changed from ERROR to WARN: This is a recoverable state (unsealed tail)
                ALOG_WARN("File {}: {} bytes of incomplete data at EOF", src_file_id, ctx.decode_buffer.Remaining());
            }
            break;
        }

        file_read_pos += bytes_read;

        if (ctx.decode_buffer.Remaining() > ctx.limits.max_decode_buffer)
        {
            ALOG_ERROR("Buffer overflow: {}MB", ctx.decode_buffer.Remaining() / 1024 / 1024);
            co_return std::unexpected(Error{ErrorCategory::kApplication, kIoDataCorrupted});
        }

        // FIX: Check the result of parsing. If it fails, assume we hit the unsealed tail.
        auto parse_result = co_await ParseEntriesFromBuffer(ctx, file_read_pos, src_file_id, dst_data_fd, dst_hint_fd);

        if (!parse_result.has_value())
        {
            // Logic: If we encounter a deserialization error, it likely means we hit the zero-filled
            // or garbage tail of an unsealed file. We stop processing this file but DO NOT return an error.
            // This allows the compaction to proceed with the valid entries we've found so far.
            ALOG_WARN("Compaction of file {} stopped early due to data end/corruption: {}", src_file_id,
                      parse_result.error());
            break;
        }

        if (ctx.decode_buffer.ShouldCompact())
        {
            ctx.decode_buffer.Compact();
        }
    }

    co_return {};
}

Task<Result<void>> ParseEntriesFromBuffer(CompactionContext& ctx, const uint64_t file_read_pos,
                                          const uint64_t src_file_id, const int dst_data_fd, const int dst_hint_fd)
{
    while (true)
    {
        auto readable = ctx.decode_buffer.ReadableSpan();
        auto entry_result = DataEntry::Deserialize(readable);

        if (!entry_result.has_value())
        {
            if (entry_result.error().value == kIoNeedMoreData)
                break;
            co_return std::unexpected(entry_result.error());
        }

        const auto& data_entry = entry_result.value();

        // Calculate entry offset in file
        const uint64_t entry_offset = file_read_pos - ctx.decode_buffer.Remaining();
        const uint64_t decoded_size = readable.size_bytes();

        ProcessParsedEntry(ctx, data_entry, decoded_size, entry_offset, src_file_id, readable);

        ctx.decode_buffer.Advance(decoded_size);

        if (ShouldFlushBatch(ctx))
        {
            KIO_TRY(co_await CommitBatch(ctx, dst_data_fd, dst_hint_fd));
        }
    }

    co_return {};
}

void ProcessParsedEntry(CompactionContext& ctx, const DataEntry& data_entry, const uint64_t decoded_size,
                        uint64_t entry_offset, uint64_t src_file_id, std::span<const char> readable_data)
{
    if (!IsLiveEntry(ctx, data_entry, entry_offset, src_file_id))
    {
        ctx.entries_discarded++;
        return;
    }

    ctx.entries_kept++;

    std::span<const char> raw_entry = readable_data.subspan(0, decoded_size);

    ValueLocation const new_loc{.file_id = ctx.dst_file_id,
                                .offset = ctx.current_data_offset + ctx.data_batch.size(),
                                .total_size = static_cast<uint32_t>(decoded_size),
                                .timestamp_ns = data_entry.GetTimestamp()};

    const HintEntry hint{new_loc.timestamp_ns, new_loc.offset, new_loc.total_size,
                         std::string(data_entry.GetKeyView())};

    ctx.data_batch.insert(ctx.data_batch.end(), raw_entry.begin(), raw_entry.end());
    auto serialized_entry = hint.Serialize();
    ctx.hint_batch.insert(ctx.hint_batch.end(), serialized_entry.begin(), serialized_entry.end());
    ctx.keydir_batch.emplace_back(std::string(data_entry.GetKeyView()), new_loc, src_file_id, entry_offset);
}

Task<Result<void>> CommitBatch(CompactionContext& ctx, const int dst_data_fd, const int dst_hint_fd)
{
    if (ctx.data_batch.empty())
    {
        co_return {};
    }

    KIO_TRY(co_await ctx.io_worker.AsyncWriteExact(dst_data_fd, std::span(ctx.data_batch)));
    KIO_TRY(co_await ctx.io_worker.AsyncWriteExact(dst_hint_fd, std::span(ctx.hint_batch)));

    ctx.bytes_written += ctx.data_batch.size();

    auto& dst_stats = ctx.stats.data_files[ctx.dst_file_id];
    dst_stats.total_bytes += ctx.data_batch.size();
    dst_stats.live_bytes += ctx.data_batch.size();
    dst_stats.live_entries += ctx.keydir_batch.size();

    // Update KeyDir with CAS, adjust stats if CAS fails
    for (const auto& update : ctx.keydir_batch)
    {
        if (!UpdateKeydirIfMatches(ctx, update.key, update.new_loc, update.expected_file_id, update.expected_offset))
        {
            dst_stats.live_bytes -= update.new_loc.total_size;
            dst_stats.live_entries--;
        }
    }

    ctx.current_data_offset += ctx.data_batch.size();
    ctx.current_hint_offset += ctx.hint_batch.size();

    ResetBatches(ctx);
    co_return {};
}

bool ShouldFlushBatch(const CompactionContext& ctx)
{
    return ctx.data_batch.size() >= ctx.config.write_buffer_size ||
           ctx.hint_batch.size() >= ctx.limits.max_hint_batch || ctx.keydir_batch.size() >= ctx.limits.max_keydir_batch;
}

void ResetBatches(CompactionContext& ctx)
{
    ctx.data_batch.clear();
    ctx.hint_batch.clear();
    ctx.keydir_batch.clear();
}

Task<Result<void>> CleanupSourceFiles(const CompactionContext& ctx, const uint64_t src_file_id)
{
    co_await ctx.io_worker.AsyncUnlinkAt(AT_FDCWD, GetDataFilePath(ctx, src_file_id), 0);
    co_await ctx.io_worker.AsyncUnlinkAt(AT_FDCWD, GetHintFilePath(ctx, src_file_id), 0);
    co_return {};
}

bool IsLiveEntry(const CompactionContext& ctx, const DataEntry& entry, const uint64_t old_offset,
                 const uint64_t old_file_id)
{
    const auto it = ctx.keydir.find(Hash(entry.GetKeyView()));
    if (it == ctx.keydir.end())
    {
        return false;
    }

    return it->second.timestamp_ns == entry.GetTimestamp() && it->second.offset == old_offset &&
           it->second.file_id == old_file_id;
}

bool UpdateKeydirIfMatches(const CompactionContext& ctx, std::string_view key, const ValueLocation& new_loc,
                           const uint64_t expected_file_id, const uint64_t expected_offset)
{
    const auto it = ctx.keydir.find(Hash(key));
    if (it == ctx.keydir.end())
    {
        return false;
    }

    if (it->second.file_id != expected_file_id || it->second.offset != expected_offset)
    {
        return false;
    }

    it->second = new_loc;
    return true;
}

std::filesystem::path GetDataFilePath(const CompactionContext& ctx, uint64_t file_id)
{
    return ctx.config.directory / std::format("partition_{}/data_{}.db", ctx.partition_id, file_id);
}

std::filesystem::path GetHintFilePath(const CompactionContext& ctx, uint64_t file_id)
{
    return ctx.config.directory / std::format("partition_{}/hint_{}.ht", ctx.partition_id, file_id);
}
}  // namespace detail

}  // namespace bitcask::compactor
