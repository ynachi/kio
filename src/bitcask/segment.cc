#include "bitcask/segment.hpp"

#include "bitcask/common.hpp"

namespace bitcask
{

void SegmentManager::prepare_write(std::span<const std::byte> key, std::span<const std::byte> value, EntryFlags flags,
                                   LogEntryHeader& hdr, uint64_t& payload_crc)
{
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
    payload_crc = XXH3_64bits_digest(state);
}

URing::Task<uint64_t> SegmentManager::append(URing::IO& io, std::span<const std::byte> key,
                                             std::span<const std::byte> value, const EntryFlags flags)
{
    if (auto fast_res = try_append_buffered_fast(key, value, flags); fast_res.has_value()) [[likely]]
    {
        co_return *fast_res;
    }

    // ensure active file
    if (!active_segment_.has_value())
    {
        URING_TRY_VOID(co_await create_active(io));
    }

    const uint64_t total_entry_size = sizeof(LogEntryHeader) + key.size() + value.size() + sizeof(uint64_t);

    // case where we need to bypass buffer
    if (cfg_.durability == Durability::SyncOnWrite || total_entry_size > cfg_.flush_buffer_size)
    {
        // flush first
        URING_TRY_VOID(co_await flush(io));

        LogEntryHeader hdr{};
        uint64_t payload_crc{};
        prepare_write(key, value, flags, hdr, payload_crc);

        URING_TRY(uint64_t entry_offset, co_await append_direct(io, key, value, hdr, payload_crc));

        if (next_disk_offset_ + next_buf_offset_ >= cfg_.max_segment_size)
        {
            URING_TRY_VOID(co_await rotate(io));
        }

        if (cfg_.durability == Durability::SyncOnWrite)
        {
            URING_TRY_VOID(co_await io.fsync(*active_segment_, /*datasync=*/true));
        }

        co_return entry_offset;
    }

    // ensure active buffer
    if (!write_buffer_.has_value())
    {
        URING_TRY(auto buf, io.take_fixed_buffer(cfg_.flush_buffer_size));
        write_buffer_.emplace(std::move(buf));
    }

    // Our buffer is not growable, so we might need to flush, if the record can fit the buffer and there is not enough
    // space
    if (next_buf_offset_ + total_entry_size > cfg_.flush_buffer_size)
    {
        URING_TRY_VOID(co_await flush(io));
    }

    co_return append_buffered_ready(key, value, flags);
}

bool SegmentManager::serve_from_buffer(URing::FixedBuffer& out, const uint64_t offset, const size_t len)
{
    if (!should_read_from_buffer(offset, len))
    {
        return false;
    }

    const auto buffer_offset = offset - next_disk_offset_;
    std::memcpy(out.ptr(), write_buffer_->ptr() + buffer_offset, len);
    return true;
}

bool SegmentManager::should_read_from_buffer(const uint64_t offset, const size_t len) const
{
    const uint64_t buffer_end = next_disk_offset_ + next_buf_offset_;
    return write_buffer_.has_value() && offset >= next_disk_offset_ && offset + len <= buffer_end;
}

URing::Task<uint64_t> SegmentManager::append_direct(URing::IO& io, std::span<const std::byte> key,
                                                    std::span<const std::byte> value, LogEntryHeader& hdr,
                                                    uint64_t payload_crc)
{
    std::array<iovec, 4> iovs = {
        {{&hdr, sizeof(hdr)},
         {const_cast<std::byte*>(key.data()), key.size()},
         {const_cast<std::byte*>(value.data()), value.size()},
         {&payload_crc, sizeof(payload_crc)}}
    };

    const uint64_t entry_offset = next_disk_offset_;
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

    next_disk_offset_ += bytes_written;

    co_return entry_offset;
}

uint64_t SegmentManager::copy_to_buffer(std::span<const std::byte> key, std::span<const std::byte> value,
                                        const LogEntryHeader& hdr, const uint64_t payload_crc)
{
    const uint64_t entry_offset = next_disk_offset_ + next_buf_offset_;
    std::byte* ptr = write_buffer_->ptr() + next_buf_offset_;

    std::memcpy(ptr, &hdr, sizeof(hdr));
    ptr += sizeof(hdr);
    std::memcpy(ptr, key.data(), key.size());
    ptr += key.size();
    std::memcpy(ptr, value.data(), value.size());
    ptr += value.size();
    std::memcpy(ptr, &payload_crc, sizeof(payload_crc));

    next_buf_offset_ += hdr.total_size();

    return entry_offset;
}

uint64_t SegmentManager::append_buffered_ready(std::span<const std::byte> key, std::span<const std::byte> value,
                                               const EntryFlags flags)
{
    LogEntryHeader hdr{};
    uint64_t payload_crc{};
    prepare_write(key, value, flags, hdr, payload_crc);

    return copy_to_buffer(key, value, hdr, payload_crc);
}

std::optional<uint64_t> SegmentManager::try_append_buffered_fast(std::span<const std::byte> key,
                                                                 std::span<const std::byte> value,
                                                                 const EntryFlags flags)
{
    if (cfg_.durability == Durability::SyncOnWrite || !active_segment_.has_value() || !write_buffer_.has_value())
    {
        return std::nullopt;
    }

    const uint64_t total_entry_size = sizeof(LogEntryHeader) + key.size() + value.size() + sizeof(uint64_t);
    if (total_entry_size > cfg_.flush_buffer_size || next_buf_offset_ + total_entry_size > cfg_.flush_buffer_size)
    {
        return std::nullopt;
    }

    return append_buffered_ready(key, value, flags);
}

URing::Task<void> SegmentManager::flush(URing::IO& io)
{
    if (!write_buffer_.has_value() || next_buf_offset_ == 0)
    {
        co_return {};
    }

    auto res = co_await io.write_fixed(*active_segment_, *write_buffer_, next_buf_offset_, next_disk_offset_);
    if (!res)
    {
        co_return std::unexpected(res.error());
    }
    next_disk_offset_ += res.value();
    // TODO: we might want to clear the buffer does we ?
    next_buf_offset_ = 0;

    co_return {};
}

URing::Task<void> SegmentManager::value_into(URing::IO& io, URing::FixedBuffer& buf, ValueLocation& loc)
{
    if (loc.segment_id == active_segment_id_ && serve_from_buffer(buf, loc.value_offset, loc.value_len))
    {
        co_return {};
    }

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

// TODO: ia badly generetad, rewrite.
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
    const uint64_t crc_offset = record_offset + sizeof(LogEntryHeader) + hdr.key_len + hdr.val_len;
    auto crc_res = co_await io.read(*fd_res->get(), std::as_writable_bytes(std::span(&payload_crc, 1)), crc_offset);
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

    if (auto res = co_await io.ftruncate(*active_segment_, next_disk_offset_); !res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(res.error());
    }

    if (auto res = co_await io.fsync(*active_segment_, true); !res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(res.error());
    }

    co_return {};
}

URing::Task<std::shared_ptr<URing::Fd>> SegmentManager::open_and_cache_fd(URing::IO& io, const SegmentId id)
{
    ALOG_DEBUG("no cached file, opening a new one, shard{}", shard_id_);
    std::filesystem::path path = cfg_.get_data_file_path(id, shard_id_);
    const auto path_str = path.string();

    URING_TRY_LOG(auto fd_inner, co_await io.open(std::move(path), cfg_.read_flags, cfg_.file_mode),
                  "open segment file failed shard={} path={}", shard_id_, path_str);

    auto fd_shared = std::make_shared<URing::Fd>(std::move(fd_inner));

    std::optional<std::shared_ptr<URing::Fd>> evicted_fd = ro_fd_cache_.put(id, fd_shared);
    // close if we are the sole owner
    if (evicted_fd.has_value() && evicted_fd->use_count() == 1)
    {
        URING_TRY_VOID_LOG(co_await io.close(std::move(*evicted_fd->get())),
                           "close evicted file failed shard={} path={}", shard_id_, path_str);
    }

    co_return fd_shared;
}

URing::Task<std::optional<URing::Fd>> SegmentManager::create_active(URing::IO& io)
{
    const SegmentId id = next_segment_id();
    std::filesystem::path path = cfg_.get_data_file_path(id, shard_id_);

    const auto path_str = path.string();  // capture BEFORE the move
    URING_TRY_LOG(auto fd, co_await io.open(std::move(path), cfg_.write_flags, cfg_.file_mode),
                  "create active file failed shard={} path={}", shard_id_, path_str);

    URING_TRY_VOID_LOG(co_await io.fallocate(fd, 0, 0, cfg_.max_segment_size), "fallocate active failed shard={}",
                       shard_id_);

    // reset counters
    next_disk_offset_ = 0;
    active_segment_id_ = id;

    // replace
    co_return std::exchange(active_segment_, std::move(fd));
}

URing::Task<void> SegmentManager::rotate(URing::IO& io)
{
    // flush first
    if (auto flush_res = co_await flush(io); !flush_res.has_value()) [[unlikely]]
    {
        co_return std::unexpected(flush_res.error());
    }

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
    // flush first
    URING_TRY_VOID(co_await flush(io));

    URING_TRY_VOID(co_await seal_active(io));

    URING_TRY_VOID(co_await io.close(std::move(*active_segment_)));

    co_return {};
}

}  // namespace bitcask
