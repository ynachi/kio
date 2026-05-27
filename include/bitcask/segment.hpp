#pragma once
#include "external_libraries/xxhash/xxhash.h"
#include "uring/core/io.h"

#include <atomic>
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
    // use optional to allow replacing the active fd
    std::optional<URing::Fd> active_segment_;
    SegmentId active_segment_id_{0};
    uint64_t next_offset_{0};
    FdCache ro_fd_cache_;
    XXH3_state_t* xxh3_state_;
    uint64_t secno_{1};
    ShardId shard_id_{0};
    BitcaskConfig cfg_{};

    URing::Task<void> seal_active(URing::IO& io);
    URing::Task<std::optional<URing::Fd>> create_active(URing::IO& io);

    URing::Task<std::shared_ptr<URing::Fd>> open_and_cache_fd(URing::IO& io, SegmentId id);

public:
    // TODO: not complete yet
    SegmentManager() { xxh3_state_ = XXH3_createState(); }
    ~SegmentManager() { XXH3_freeState(xxh3_state_); }
    uint64_t next_secno() { return ++secno_; }
    uint64_t next_segment_id() { return ++active_segment_id_; }

    URing::Task<uint64_t> append(URing::IO& io, std::span<const std::byte> key, std::span<const std::byte> value,
                                 EntryFlags flags = EntryFlags::HasValue);
    // read value into buf
    URing::Task<void> value_into(URing::IO& io, std::span<std::byte> buf, ValueLocation& loc);

    // rotate active file
    URing::Task<void> rotate(URing::IO& io);
};
}  // namespace bitcask