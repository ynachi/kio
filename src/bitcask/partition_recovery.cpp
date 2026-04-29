#include "bitcask/common.hpp"
#include "bitcask/partition_recovery.hpp"

#include <ranges>

namespace bitcask
{
    namespace
    {
        kio::Result<std::pair<uint64_t, uint64_t>> RecoverDataFromBuffer(
            PartitionIO& io, kio::IoBuffer& buffer, const uint64_t file_id,
            const uint64_t file_read_position)
        {
            uint64_t entries_recovered = 0;
            uint64_t entries_skipped = 0;

            while (buffer.ReadableBytes() >= kEntryFixedHeaderSize)
            {
                const auto readable = buffer.ReadableSpan();
                auto entry_result = DataEntry::Deserialize(readable);

                if (!entry_result.has_value())
                {
                    if (entry_result.error() == kio::ParseError::Incomplete)
                    {
                        // Need more data - break and let caller read more
                        break;
                    }
                    // Actual corruption - could truncate here for crash recovery
                    return std::unexpected(entry_result.error());
                }

                auto entry = std::move(entry_result.value());

                // file_read_position = total bytes read from file so far
                // buffer.Remaining() = unprocessed bytes still in buffer
                // Therefore, the start of current readable data in the file is:
                const uint64_t entry_offset = file_read_position - buffer.ReadableBytes();

                buffer.Consume(entry.Size());

                if (entry.IsTombstone())
                {
                    io.GetKeyDir().erase(entry.GetKeyView());
                    entries_skipped++;
                    continue;
                }

                entries_recovered++;

                std::string key_copy(entry.GetKeyView());
                io.GetKeyDir().insert_or_assign(std::move(key_copy), ValueLocation{
                                                    .file_id = file_id,
                                                    .offset = entry_offset,
                                                    .total_size = entry.Size(),
                                                    .timestamp_ns = entry.GetTimestamp()
                                                });
            }
            return std::make_pair(entries_recovered, entries_skipped);
        }
    }

    kio::Task<kio::Result<uint64_t>> RecoverFromHintFile(kio::IoContext& ctx, PartitionIO& io,
                                                         const kio::FD& fh, const uint64_t file_id)
    {
        const int fd = fh.Get();
        const uint64_t file_size = KIO_CO_TRY_LOG(GetFileSize(fd));

        if (file_size == 0)
        {
            co_return {};
        }

        // Hint files are small, read entirely
        std::vector<std::byte> buffer(file_size);
        KIO_CO_TRY_LOG(co_await kio::AsyncReadExact(ctx, fd, buffer, 0));

        std::span<const std::byte> remaining(buffer);
        uint64_t entries_recovered = 0;

        while (remaining.size() >= kHintHeaderSize)
        {
            auto result = KIO_CO_TRY_LOG(HintEntry::Deserialize(remaining));

            auto [hint, consumed] = result;
            remaining = remaining.subspan(consumed);

            // Insert into the keydir (hint entries are always live - tombstones aren't in hints)
            io.GetKeyDir().insert_or_assign(
                std::move(hint.key),
                ValueLocation{
                    .file_id = file_id, .offset = hint.offset, .total_size = hint.size,
                    .timestamp_ns = hint.timestamp_ns
                });
            entries_recovered++;
        }

        co_return entries_recovered;
    }

    kio::Task<kio::Result<uint64_t>> TryRecoverFromHint(
        kio::IoContext& ctx,
        PartitionIO& io,
        const BitcaskConfig& config,
        uint64_t file_id
    )
    {
        const auto hint_path = config.directory / std::format("partition_{}/hint_{}.ht", io.PartitionID(), file_id);
        if (!std::filesystem::exists(hint_path))
        {
            ALOG_DEBUG("Hint file {} does not exist", file_id);
            co_return ErrorFromErrc(std::errc::no_such_file_or_directory);
        }

        const auto fd = KIO_CO_TRY_LOG(co_await AsyncOpen(ctx, hint_path, config.read_flags, config.file_mode));

        auto result = KIO_CO_TRY_LOG(co_await RecoverFromHintFile(ctx, io, fd, file_id));

        co_return result;
    }

    kio::Task<kio::Result<uint64_t>> RecoverFromDataFile(
        kio::IoContext& ctx,
        PartitionIO& io,
        const BitcaskConfig& config,
        const kio::FD& fh,
        uint64_t file_id
    )
    {
        const int fd = fh.Get();
        const uint64_t file_size = KIO_CO_TRY_LOG(GetFileSize(fd));

        if (file_size == 0)
        {
            co_return {};
        }

        kio::IoBuffer buffer(config.read_buffer_size);
        uint64_t file_read_position = 0;
        // TODO: for debug, will be used later as metrics
        uint64_t total_recovered = 0;
        uint64_t total_skipped = 0;

        while (file_read_position < file_size)
        {
            // Ensure we have space to read
            buffer.EnsureWritableBytes(config.read_buffer_size);
            auto writable = buffer.WritableSpan();

            const uint64_t bytes_to_read = std::min(writable.size(), file_size - file_read_position);
            const auto read_result = KIO_CO_TRY_LOG(
                co_await AsyncRead(ctx, fd, writable.subspan(0, bytes_to_read), file_read_position));

            if (read_result == 0)
            {
                break; // EOF
            }

            buffer.Commit(read_result);
            file_read_position += read_result;

            // Process entries in buffer
            auto [recovered, skipped] = KIO_CO_TRY_LOG(RecoverDataFromBuffer(io, buffer, file_id, file_read_position));

            total_recovered += recovered;
            total_skipped += skipped;
        }

        ALOG_DEBUG("Recovered {} entries ({} tombstones) from data file {}", total_recovered, total_skipped, file_id);

        co_return {};
    }

    kio::Task<kio::Result<void>> RecoverPartition(
        kio::IoContext& ctx,
        PartitionIO& io,
        PartitionStats& stats,
        const BitcaskConfig& config
    )
    {
        const auto files = io.ScanDataFiles();

        // Initialize file stats with on-disk sizes
        for (auto file_id : files)
        {
            if (const auto path = io.GetDataFilePath(file_id); std::filesystem::exists(path))
            {
                stats.data_files[file_id].total_bytes = std::filesystem::file_size(path);
            }
        }

        uint64_t max_file_id = 0;
        for (const auto file_id : files)
        {
            max_file_id = std::max(file_id, max_file_id);

            // Try the hint file first (faster)
            if (const auto hint_result = co_await TryRecoverFromHint(ctx, io, config, file_id); hint_result)
            {
                continue;
            }

            // Fall back to full data file scan
            const auto fd =
                KIO_CO_TRY(
                    co_await kio::AsyncOpen(ctx, io.GetDataFilePath(file_id), config.read_flags, config.file_mode));
            if (auto res = co_await RecoverFromDataFile(ctx, io, config, fd, file_id); !res.has_value())
            {
                ALOG_ERROR("Failed to recover data file {}: {}", file_id, res.error().message());
                // Continue with other files
            }
        }

        // Compute live stats from keydir
        for (const auto& loc : io.GetKeyDir() | std::views::values)
        {
            auto& fs = stats.data_files[loc.file_id];
            fs.live_bytes += loc.total_size;
            fs.live_entries++;
        }

        // Update file ID generator to ensure monotonicity
        if (max_file_id > 0)
        {
            const auto decoded = FileID::Decode(max_file_id);
            io.FdGen().UpdateState(decoded.timestamp_sec, decoded.sequence);
        }

        KIO_CO_TRY_LOG(co_await io.CreateAndSetActiveFile(ctx));
        co_return {};
    }
}
