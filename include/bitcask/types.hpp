#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace bitcask
{
/// Segment ID (or file ID)
using SegmentId = uint32_t;

using KeyView   = std::string_view;          // keys are just strings
using ValueView = std::span<const uint8_t>;  // values are raw bytes

/// Value location or IndexEntry in some bitcask implementations
struct ValueLocation
{
    SegmentId segment_id;
    uint64_t value;
    uint64_t size;
    uint64_t secno;
};
}