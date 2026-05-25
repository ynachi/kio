#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "fdcache.hpp"
#include "uring/core/task.hpp"
#include "uring/fd.hpp"

namespace bitcask
{
/**
┌──────────┬────────┬──────────┬──────────┬────────────┐
│ hdr_crc  │ flags  │ key_len  │ val_len  │  seq_num   │
│   4B     │  2B    │   2B     │   4B     │    8B      │
├──────────┴────────┴──────────┴──────────┴────────────┤
│                  Key bytes (variable)                │
├──────────────────────────────────────────────────────┤
│                  Value bytes (variable)              │
├──────────────────────────────────────────────────────┤
│                     val_crc  (4B)                    │
└──────────────────────────────────────────────────────┘
Header = 20 bytes (was 24, saved 4 by dropping magic)
 */

enum class EntryFlags : uint8_t
{
    Deleted = 0b00,
    HasValue = 0b01,
    Compressed = 0b10,
};

class SegmentManager
{
    std::unique_ptr<URing::Fd> active_segment_;
    std::atomic<uint64_t> next_offset{0};
    FdCache ro_fd_cache_;

public:
    URing::Task<uint64_t> append(std::span<const std::byte> key, std::span<const std::byte> value);
};
}  // namespace bitcask