#include "bitcask/segment.hpp"

#include "bitcask/common.hpp"

namespace bitcask
{

URing::Task<uint64_t> SegmentManager::append(URing::IO& io, std::span<const std::byte> key,
                                             std::span<const std::byte> value, EntryFlags flags)
{
    LogEntryHeader hdr{};

    hdr.seq_num = next_secno();
    hdr.val_len = value.size();
    hdr.key_len = key.size();
    hdr.flags = static_cast<uint16_t>(flags);

    hdr.hdr_crc = XXH3_64bits(&hdr.seq_num, 24);

    // payload_crc
    XXH3_64bits_reset(xxh3_state_);
    XXH3_64bits_update(xxh3_state_, key.data(), key.size());
    XXH3_64bits_update(xxh3_state_, value.data(), value.size());
    uint64_t payload_crc = XXH3_64bits_digest(xxh3_state_);

    // perform io
    std::array<iovec, 4> iovs = {
        {{&hdr, sizeof(hdr)},
         {const_cast<std::byte*>(key.data()), key.size()},
         {const_cast<std::byte*>(value.data()), value.size()},
         {&payload_crc, sizeof(payload_crc)}}
    };

    const auto bytes_written = URING_TRY(co_await io.writev(*active_segment_, iovs, -1));

    if (bytes_written != static_cast<int32_t>(hdr.total_size()))
    {
        // Handle partial write (though rare in io_uring with O_DIRECT/Regular files)
        co_return std::unexpected(URing::make_error_code(EIO));
    }

    // update offset
    next_offset_ += bytes_written;
    uint64_t last_offset = next_offset_;

    // TODO: We must check and initiate ROTATION here to prevent race condition
    if (next_offset_ >= cfg_.max_segment_size)
    {
        URING_TRY(co_await rotate(io));
    }

    co_return last_offset;
}

URing::Task<void> SegmentManager::value_into(URing::IO& io, std::span<std::byte> buf, ValueLocation& loc)
{
    // 1. Try to get it synchronously (Fast, no allocation, no suspension)
    auto fd = ro_fd_cache_.get(loc.segment_id);

    // 2. Fallback to slow path ONLY if necessary
    if (!fd.has_value()) [[unlikely]]
    {
        // We only suspend for the OPEN if we actually missed the cache.
        fd = URING_TRY(co_await open_and_cache_fd(io, loc.segment_id));
    }

    // 3. Issue the read (The only suspension point in the 99.9% case)
    // TODO: size check and error if not the same,
    auto res = URING_TRY(co_await io.read(*fd->get(), buf.subspan(0, loc.value_len), loc.value_offset));

    co_return {};
}

URing::Task<void> SegmentManager::seal_active(URing::IO& io)
{
    URING_TRY(co_await io.fsync(*active_segment_, true));
    URING_TRY(co_await io.ftruncate(*active_segment_, next_offset_));
    URING_TRY(co_await io.fsync(*active_segment_, true));
    co_return {};
}

URing::Task<std::shared_ptr<URing::Fd>> SegmentManager::open_and_cache_fd(URing::IO& io, SegmentId id)
{
    ALOG_DEBUG("no cached file, opening a new one, shard{}", shard_id_);
    std::filesystem::path path = cfg_.get_data_file_path(id, shard_id_);
    auto fd_inner = URING_TRY(co_await io.open(std::move(path), cfg_.read_flags, cfg_.file_mode));
    auto fd_shared = std::make_shared<URing::Fd>(std::move(fd_inner));

    std::optional<std::shared_ptr<URing::Fd>> evicted_fd = ro_fd_cache_.put(id, fd_shared);
    if (evicted_fd.has_value())
    {
        URING_TRY(co_await io.close(std::move(*evicted_fd->get())));
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
        ALOG_ERROR("failed to create active file, shard={}, err={}", shard_id_, fd_res.error().message());
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
    URING_TRY(co_await seal_active(io));

    if (std::optional<URing::Fd> fd = URING_TRY(co_await create_active(io)); fd.has_value())
    {
        URING_TRY(co_await io.close(std::move(*fd)));
    }

    co_return {};
}

}  // namespace bitcask