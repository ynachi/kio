#pragma once
#define XXH_INLINE_ALL
#include "external_libraries/xxhash/xxhash.h"
#include "uring/core/io.h"

#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

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

    /**
     * @brief Append a key-value pair to the active segment.
     *
     * @warning If passing a raw C-string or a literal to the 'key' or 'value' span,
     * beware of null termination. std::span("key") includes the '\0',
     * while std::string_view("key") does not. Use the range overloads
     * for safe string/literal handling.
     */
    URing::Task<uint64_t> append(URing::IO& io, std::span<const std::byte> key, std::span<const std::byte> value,
                                 EntryFlags flags = EntryFlags::HasValue);

    /**
     * @brief Generic overload that accepts any contiguous ranges (string, vector, literal).
     * Automatically converts input to byte spans and ensures safe string handling.
     */
    template <std::ranges::contiguous_range K, std::ranges::contiguous_range V>
    URing::Task<uint64_t> append(URing::IO& io, const K& key, const V& value, EntryFlags flags = EntryFlags::HasValue)
    {
        // Internal helper to handle the common footgun of string literals in spans
        auto to_byte_span = []<typename R>(const R& range)
        {
            if constexpr (std::is_convertible_v<R, std::string_view>)
            {
                // Correctly handles literals/strings by excluding the null terminator
                return std::as_bytes(std::span(std::string_view(range)));
            }
            else
            {
                return std::as_bytes(std::span(range));
            }
        };

        return append(io, to_byte_span(key), to_byte_span(value), flags);
    }
    // read value into buf
    URing::Task<void> value_into(URing::IO& io, URing::FixedBuffer& buf, ValueLocation& loc);

    /**
     * @brief Verify the integrity of an entry at a specific offset.
     * @param record_offset The start of the record (where the header begins).
     */
    URing::Task<void> verify_entry(URing::IO& io, SegmentId segment_id, uint64_t record_offset);

    // rotate active file
    URing::Task<void> rotate(URing::IO& io);

    URing::Task<void> close(URing::IO& io);
};
}  // namespace bitcask
