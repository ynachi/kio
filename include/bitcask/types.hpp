#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace bitcask
{
constexpr size_t kDiskEntryHeaderSize = 32;

/// Segment ID (or file ID)
using SegmentId = uint32_t;

using ShardId = uint32_t;

using KeyView = std::string_view;            // keys are just strings
using ValueView = std::span<const uint8_t>;  // values are raw bytes

/// Value location or IndexEntry in some bitcask implementations
struct ValueLocation
{
    SegmentId segment_id;
    uint64_t value_len;
    uint64_t total_len;
    uint64_t value_offset;
    uint64_t secno;
};

#pragma pack(push, 1)
struct LogEntryHeader
{
    uint64_t hdr_crc;
    uint64_t seq_num;
    uint32_t val_len;
    uint16_t key_len;
    uint16_t flags;
    // reserved also allow us to have a 32B header
    uint64_t reserved = 0;

    size_t total_size() const { return sizeof(LogEntryHeader) + key_len + val_len + sizeof(uint64_t); }
};
#pragma pack(pop)

}  // namespace bitcask