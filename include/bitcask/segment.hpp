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
    uint64_t next_disk_offset_{0};
    uint64_t next_buf_offset_{0};
    std::pmr::unsynchronized_pool_resource cache_pool_;
    // take that buffer from the pool of the IO
    // There is an invariant: while append allow to pass the reference of an IO
    // within a shard, IO SHOULD not be switched.
    // TODO: we could enforce it by making each shard own an IO, lets see
    std::optional<URing::FixedBuffer> write_buffer_{std::nullopt};
    FdCache ro_fd_cache_;
    std::unique_ptr<XXH3_state_t, XXH3Deleter> xxh3_state_;
    uint64_t secno_;
    ShardId shard_id_;

    //
    // Helper methods
    //
    URing::Task<void> seal_active(URing::IO& io);
    URing::Task<std::optional<URing::Fd>> create_active(URing::IO& io);
    URing::Task<std::shared_ptr<URing::Fd>> open_and_cache_fd(URing::IO& io, SegmentId id);
    // append directly without buffering
    URing::Task<uint64_t> append_direct(URing::IO& io, std::span<const std::byte> key, std::span<const std::byte> value,
                                        LogEntryHeader& hdr, uint64_t payload_crc);
    void prepare_write(std::span<const std::byte> key, std::span<const std::byte> value, EntryFlags flags,
                       LogEntryHeader& hdr, uint64_t& payload_crc);

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

    /// @brief Append a key-value record to the active segment.
    ///
    /// @param io The io_uring context
    /// @param key The record key (must outlive the call)
    /// @param value The record value (must outlive the call)
    /// @param flags
    /// @param durability Durability guarantee for this write (default: Buffered)
    /// @return Task<uint64_t> The file offset where the record was written
    ///
    /// @durability_contract
    /// - Read-Your-Writes: Data is immediately visible to reads at the returned offset,
    ///   even if not yet flushed to disk. Reads check the write buffer first.
    /// - Durability::None: Data handed to OS page cache. May be lost on crash.
    /// - Durability::Buffered: Data buffered in user-space, flushed on size/timer.
    ///   May be lost on crash if not yet flushed. Default mode.
    /// - Durability::SyncOnWrite: fsync() called after write. Survives crash.
    ///   Adds ~1-10ms latency per write.
    /// - Crash Recovery: After unexpected shutdown, only data up to the last
    ///   successful fsync() is guaranteed. Use last_synced_offset() for checkpointing.
    ///
    /// @buffer_behavior
    /// - Max buffer size: configured via set_flush_policy() (default: 4MB)
    /// - Flush timer: configured via set_flush_policy() (default: 100ms)
    /// - Backpressure: If buffer full, append() suspends until space available
    ///
    /// @thread_safety
    /// - SegmentManager is single-threaded per shard. Do not call append()
    ///   concurrently from multiple threads without external synchronization.
    URing::Task<uint64_t> append(URing::IO& io, std::span<const std::byte> key, std::span<const std::byte> value,
                                 EntryFlags flags = EntryFlags::HasValue);

    /// @brief Generic overload that accepts any contiguous ranges (string, vector, literal).
    ///
    /// Automatically converts input to byte spans and ensures safe string handling.
    template <std::ranges::contiguous_range K, std::ranges::contiguous_range V>
    URing::Task<uint64_t> append(URing::IO& io, const K& key, const V& value,
                                 const EntryFlags flags = EntryFlags::HasValue)
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

    /// read value into a fixed buf
    URing::Task<void> value_into(URing::IO& io, URing::FixedBuffer& buf, ValueLocation& loc);

    /// @brief Verify the integrity of an entry at a specific offset.
    ///
    /// @param io
    /// @param segment_id
    /// @param record_offset The start of the record (where the header begins).
    // TODO, rewrite
    URing::Task<void> verify_entry(URing::IO& io, SegmentId segment_id, uint64_t record_offset);

    // rotate active file
    URing::Task<void> rotate(URing::IO& io);

    // Push buffered data to OS page cache (non-blocking, no fsync)
    URing::Task<void> flush(URing::IO& io);

    // Force fsync to physical media (blocking until disk ACK)
    URing::Task<void> sync(URing::IO& io);

    URing::Task<void> close(URing::IO& io);
};
}  // namespace bitcask
