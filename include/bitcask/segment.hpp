#pragma once
#include "external_libraries/xxhash/xxhash.h"
#include "uring/core/io.h"

#include <memory>
#include <memory_resource>
#include <optional>

#include "common.hpp"
#include "fdcache.hpp"
#include "uring/core/task.hpp"
#include "uring/fd.hpp"

namespace bitcask
{
/**
┌──────────────────────────────────────────────────────┐
│                  hdr_crc (8B)                        │
├──────────────────────────────────────────────────────┤
│                  seq_num (8B)                        │
├──────────┬──────────┬──────────┬─────────────────────┤
│ val_len  │ key_len  │  flags   │      reserved       │
│   4B     │   2B     │    2B    │         8B          │
├──────────┴──────────┴──────────┴─────────────────────┤
│                  Key bytes (variable)                │
├──────────────────────────────────────────────────────┤
│                  Value bytes (variable)              │
├──────────────────────────────────────────────────────┤
│                  val_crc (8B)                        │
└──────────────────────────────────────────────────────┘
Header = 32 bytes (Aligned for cache-line efficiency)
 */

enum class EntryFlags : uint16_t
{
    Deleted = 0b00,
    HasValue = 0b01,
    Compressed = 0b10,
};

// Single threaded segment write/read and active file rotation
class SegmentManager
{
    struct XXH3Deleter
    {
        void operator()(XXH3_state_t* state) const
        {
            if (state != nullptr)
            {
                XXH3_freeState(state);
            }
        }
    };

    // use optional to allow replacing the active fd
    BitcaskConfig cfg_{};
    std::optional<URing::Fd> active_segment_{std::nullopt};
    SegmentId active_segment_id_;
    uint64_t next_offset_{0};
    std::pmr::unsynchronized_pool_resource cache_pool_;
    FdCache ro_fd_cache_;
    std::unique_ptr<XXH3_state_t, XXH3Deleter> xxh3_state_;
    uint64_t secno_;
    ShardId shard_id_;

    URing::Task<void> seal_active(URing::IO& io);
    URing::Task<std::optional<URing::Fd>> create_active(URing::IO& io);

    URing::Task<std::shared_ptr<URing::Fd>> open_and_cache_fd(URing::IO& io, SegmentId id);

public:
    // TODO: not complete yet
    SegmentManager(const SegmentManager&) = delete;
    SegmentManager& operator=(const SegmentManager&) = delete;

    // Allow moving if necessary
    SegmentManager(SegmentManager&&) = delete;
    SegmentManager& operator=(SegmentManager&&) = delete;

    /// @brief Construct a SegmentManager for a specific shard.
    /// @param shard_id The ID of the shard this manager handles.
    /// @param cfg Configuration for the Bitcask instance.
    /// @param last_secno The last used sequence number.
    /// @param last_segment_id The last used segment ID.
    SegmentManager(ShardId shard_id, BitcaskConfig cfg, uint64_t last_secno, SegmentId last_segment_id);

    ~SegmentManager() noexcept;
    uint64_t next_secno() { return ++secno_; }
    uint64_t next_segment_id() { return ++active_segment_id_; }

    URing::Task<uint64_t> append(URing::IO& io, std::span<const std::byte> key, std::span<const std::byte> value,
                                 EntryFlags flags = EntryFlags::HasValue);
    // read value into buf
    URing::Task<void> value_into(URing::IO& io, URing::FixedBuffer& buf, ValueLocation& loc);

    // rotate active file
    URing::Task<void> rotate(URing::IO& io);

    URing::Task<void> close(URing::IO& io);
};
}  // namespace bitcask