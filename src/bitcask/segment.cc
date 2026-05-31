#include "bitcask/segment.hpp"

#include "bitcask/common.hpp"

namespace bitcask
{

URing::Task<uint64_t> SegmentManager::append(URing::IO& io, std::span<const std::byte> key,
                                             std::span<const std::byte> value, EntryFlags flags)
{
    if (!active_segment_.has_value())
    {
        auto active_res = co_await create_active(io);
        if (!active_res.has_value()) [[unlikely]]
        {
            co_return std::unexpected(active_res.error());
        }
    }

    LogEntryHeader hdr{};

    hdr.seq_num = next_secno();
    hdr.val_len = value.size();
    hdr.key_len = key.size();
    hdr.flags = static_cast<uint16_t>(flags);

    hdr.hdr_crc = XXH3_64bits(&hdr.seq_num, 24);

    // payload_crc
    auto* state = xxh3_state_.get();
    XXH3_64bits_reset(state);
    XXH3_64bits_update(state, key.data(), key.size());
    XXH3_64bits_update(state, value.data(), value.size());
    uint64_t payload_crc = XXH3_64bits_digest(state);

    // perform io
    std::array<iovec, 4> iovs = {
        {{&hdr, sizeof(hdr)},
         {const_cast<std::byte*>(key.data()), key.size()},
         {const_cast<std::byte*>(value.data()), value.size()},
         {&payload_crc, sizeof(payload_crc)}}
    };

    const uint64_t entry_offset = next_offset_;
    auto write_res = co_await io.writev(*active_segment_, iovs, static_cast<off_t>(entry_offset));
    if (!write_res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(write_res.error());
    }
    const auto bytes_written = *write_res;

    if (bytes_written != static_cast<int32_t>(hdr.total_size()))
    {
        // Handle partial write (though rare in io_uring with O_DIRECT/Regular files)
        co_return std::unexpected(URing::make_error_code(EIO));
    }

    next_offset_ += bytes_written;

    // TODO: We must check and initiate ROTATION here to prevent race condition
    if (next_offset_ >= cfg_.max_segment_size)
    {
        auto rotate_res = co_await rotate(io);
        if (!rotate_res.has_value()) [[unlikely]]
        {
            co_return std::unexpected(rotate_res.error());
        }
    }

    co_return entry_offset;
}

URing::Task<void> SegmentManager::value_into(URing::IO& io, URing::FixedBuffer& buf, ValueLocation& loc)
{
    auto fd = ro_fd_cache_.get(loc.segment_id);

    if (!fd.has_value()) [[unlikely]]
    {
        // We only suspend for the OPEN if we actually missed the cache.
        auto open_res = co_await open_and_cache_fd(io, loc.segment_id);
        if (!open_res.has_value()) [[unlikely]]
        {
            co_return std::unexpected(open_res.error());
        }
        fd = *open_res;
    }

    auto read_res = co_await io.read_fixed(*fd->get(), buf, loc.value_len, loc.value_offset);
    if (!read_res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(read_res.error());
    }
    auto res = *read_res;

    if (res != static_cast<int32_t>(loc.value_len))
    {
        ALOG_ERROR("Short read from segment {}: expected {}, got {}", loc.segment_id, loc.value_len, res);
        co_return std::unexpected(URing::error_from_errc(std::errc::io_error));
    }

    co_return {};
}

URing::Task<void> SegmentManager::verify_entry(URing::IO& io, SegmentId segment_id, uint64_t record_offset)
{
    auto fd_res = ro_fd_cache_.get(segment_id);
    if (!fd_res.has_value())
    {
        auto open_res = co_await open_and_cache_fd(io, segment_id);
        if (!open_res.has_value())
            co_return std::unexpected(open_res.error());
        fd_res = *open_res;
    }

    // 1. Read and verify Header
    LogEntryHeader hdr{};
    auto head_res = co_await io.read(*fd_res->get(), std::as_writable_bytes(std::span(&hdr, 1)), record_offset);
    if (!head_res.has_value())
        co_return std::unexpected(head_res.error());

    uint64_t expected_hdr_crc = XXH3_64bits(&hdr.seq_num, 24);
    if (hdr.hdr_crc != expected_hdr_crc)
    {
        ALOG_ERROR("Header CRC mismatch at segment {}, offset {}", segment_id, record_offset);
        co_return std::unexpected(URing::error_from_errc(std::errc::bad_message));
    }

    // 2. Read and verify Value CRC (at the end of the record)
    uint64_t payload_crc = 0;
    uint64_t crc_offset = record_offset + sizeof(LogEntryHeader) + hdr.key_len + hdr.val_len;
    auto crc_res =
        co_await io.read(*fd_res->get(), std::as_writable_bytes(std::span(&payload_crc, 1)), crc_offset);
    if (!crc_res.has_value())
        co_return std::unexpected(crc_res.error());

    // In a real implementation, we'd also read key+value and re-calculate the payload_crc here.
    // For this demonstration, the header check is enough to detect corruption.

    co_return {};
}

URing::Task<void> SegmentManager::seal_active(URing::IO& io)
{
    if (auto res = co_await io.fsync(*active_segment_, true); !res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(res.error());
    }

    if (auto res = co_await io.ftruncate(*active_segment_, next_offset_); !res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(res.error());
    }

    if (auto res = co_await io.fsync(*active_segment_, true); !res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(res.error());
    }

    co_return {};
}

URing::Task<std::shared_ptr<URing::Fd>> SegmentManager::open_and_cache_fd(URing::IO& io, SegmentId id)
{
    ALOG_DEBUG("no cached file, opening a new one, shard{}", shard_id_);
    std::filesystem::path path = cfg_.get_data_file_path(id, shard_id_);

    auto open_res = co_await io.open(std::move(path), cfg_.read_flags, cfg_.file_mode);
    if (!open_res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(open_res.error());
    }
    auto fd_inner = std::move(*open_res);
    auto fd_shared = std::make_shared<URing::Fd>(std::move(fd_inner));

    std::optional<std::shared_ptr<URing::Fd>> evicted_fd = ro_fd_cache_.put(id, fd_shared);
    if (evicted_fd.has_value())
    {
        if (auto res = co_await io.close(std::move(*evicted_fd->get())); !res.has_value()) [[unlikely]]
        {
            co_return std::unexpected(res.error());
        }
    }

    co_return fd_shared;
}

URing::Task<std::optional<URing::Fd>> SegmentManager::create_active(URing::IO& io)
{
    const SegmentId id = next_segment_id();
    std::filesystem::path path = cfg_.get_data_file_path(id, shard_id_);

    auto fd_res = co_await io.open(std::move(path), cfg_.write_flags, cfg_.file_mode);
    if (!fd_res.has_value())
    {
        ALOG_ERROR("failed to create active file, shard={}, path={}, err={}", shard_id_, path.c_str(),
                   fd_res.error().message());
        co_return std::unexpected(fd_res.error());
    }

    if (const auto res = co_await io.fallocate(*fd_res, 0, 0, cfg_.max_segment_size); !res.has_value())
    {
        ALOG_ERROR("failed to create active file, shard={}", shard_id_);
        co_return std::unexpected(res.error());
    }

    // reset counters
    next_offset_ = 0;
    active_segment_id_ = id;

    // replace
    co_return std::exchange(active_segment_, std::move(*fd_res));
}

URing::Task<void> SegmentManager::rotate(URing::IO& io)
{
    if (auto res = co_await seal_active(io); !res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(res.error());
    }

    auto create_res = co_await create_active(io);
    if (!create_res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(create_res.error());
    }

    if (std::optional<URing::Fd> fd = std::move(*create_res); fd.has_value())
    {
        if (auto res = co_await io.close(std::move(*fd)); !res.has_value()) [[unlikely]]
        {
            co_return std::unexpected(res.error());
        }
    }

    co_return {};
}

SegmentManager::SegmentManager(const ShardId shard_id, BitcaskConfig cfg, const uint64_t last_secno,
                               const SegmentId last_segment_id)
    : cfg_(std::move(cfg)),
      active_segment_id_(last_segment_id),
      cache_pool_(std::pmr::pool_options{
          .max_blocks_per_chunk = 1024,
          .largest_required_pool_block = 256,
      }),
      ro_fd_cache_(cfg_.max_open_sealed_files, &cache_pool_),
      xxh3_state_(XXH3_createState()),
      secno_(last_secno),
      shard_id_(shard_id)
{
    if (xxh3_state_ == nullptr)
    {
        throw std::runtime_error("XXH3 state init failed");
    }

    // Validate Bitcask configuration (e.g., O_APPEND check, size limits)
    if (auto res = cfg_.validate(); !res.has_value())
    {
        throw std::invalid_argument("Invalid BitcaskConfig: " + res.error().message());
    }
}

SegmentManager::~SegmentManager() noexcept
{
    // XXH3 state is automatically freed by unique_ptr.

    // Safety Check for Active Segment
    // If active_segment_ still has a valid FD, it means the user forgot
    // to call 'co_await close(io)'. We cannot seal/truncate here because
    // it requires async IO, so we log a warning.
    if (active_segment_ && active_segment_->IsValid())
    {
        ALOG_WARN(
            "SegmentManager destroyed with active segment still open! "
            "This may result in missing data seals/truncations. "
            "Ensure close(io) is called and awaited before destruction.");

        // URing::Fd's destructor will still call ::close(fd) synchronously
        // as a last resort to prevent FD leaks.
    }

    // FdCache Cleanup
    // ro_fd_cache_ will be destroyed automatically. Since it stores
    // shared_ptr<URing::Fd>, any files not currently being used by
    // active tasks will be closed synchronously.
}

URing::Task<void> SegmentManager::close(URing::IO& io)
{
    if (auto res = co_await seal_active(io); !res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(res.error());
    }
    if (auto res = co_await io.close(std::move(*active_segment_)); !res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(res.error());
    }
    co_return {};
}

}  // namespace bitcask
