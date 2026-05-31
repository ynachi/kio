#pragma once

#include <initializer_list>
#include <span>

#include <liburing.h>

#include "uring/error.hpp"
#include "uring/logger.hpp"

namespace URing
{

static constexpr signed kBufferAlignment = 4096;
static constexpr uint32_t kInvalidBufIndex = static_cast<uint32_t>(-1);

struct BucketConfig
{
    size_t size;
    size_t count;
};

class FixedBufferPool;

class FixedBuffer
{
    uint32_t index_ = kInvalidBufIndex;  ///< Global index in io_uring iovec array
    uint32_t bucket_id_ = 0;             ///< Which bucket this buffer belongs to
    std::span<std::byte> view_;          ///< View into the pinned memory region
    FixedBufferPool* pool_ = nullptr;

public:
    ///@brief
    /// @param index the global index in io_uring iovec array
    /// @param bucket_id The bucket which this buffer belongs to
    /// @param view
    /// @param pool
    FixedBuffer(const uint32_t index, const uint32_t bucket_id, std::span<std::byte> view,
                FixedBufferPool* pool) noexcept
        : index_(index), bucket_id_(bucket_id), view_(view), pool_(pool)
    {
    }

    FixedBuffer(const FixedBuffer&) = delete;

    FixedBuffer(FixedBuffer&& other) noexcept;

    FixedBuffer& operator=(FixedBuffer&& other) noexcept;

    ~FixedBuffer() noexcept { release(); }

    /// Return buffer to pool (idempotent)
    void release() noexcept;

    [[nodiscard]] uint32_t index() const noexcept { return index_; }
    [[nodiscard]] std::span<std::byte> data() noexcept { return view_; }
    [[nodiscard]] std::span<const std::byte> data() const noexcept { return view_; }

    [[nodiscard]] std::byte* ptr() noexcept { return view_.data(); }
    [[nodiscard]] const std::byte* ptr() const noexcept { return view_.data(); }
    [[nodiscard]] size_t size() const noexcept { return view_.size(); }
    [[nodiscard]] bool valid() const noexcept { return pool_ != nullptr && index_ != kInvalidBufIndex; }
};

// ============================================================================
// FixedBufferPool: single-threaded fixed buffer manager
// ============================================================================
class FixedBufferPool
{
    struct Bucket
    {
        size_t slot_size;                                    ///< Size of each slot in this bucket
        uint32_t start_index;                                ///< First global index for this bucket
        std::vector<uint32_t> free_stack;                    ///< LIFO freelist (single-threaded)
        std::unique_ptr<std::byte[], void (*)(void*)> slab;  ///< memory region

        Bucket(size_t size, size_t count, uint32_t start);

        [[nodiscard]] Result<uint32_t> pop() noexcept;

        /// Return a slot to the freelist
        void push(const uint32_t global_idx) noexcept { free_stack.push_back(global_idx); }
    };

    std::vector<Bucket> buckets_;       ///< Size-sorted buckets
    std::vector<iovec> global_iovecs_;  ///< Flattened array for io_uring_register_buffers
    bool registered_{false};            ///< Track registration state

public:
    /// Construct pool from bucket configurations
    /// @param configs List of {slot_size, count} pairs (will be sorted internally)
    explicit FixedBufferPool(std::initializer_list<BucketConfig> configs);

    /// Acquire the smallest buffer >= requested size
    /// @param size Minimum buffer size needed
    /// @return FixedBuffer on success, PoolError on failure
    [[nodiscard]] Result<FixedBuffer> take(size_t size) noexcept;

    /// Return buffer to pool (called automatically by FixedBuffer destructor)
    /// @param global_index Index returned by take()
    /// @param bucket_id Bucket ID stored in FixedBuffer
    void release(uint32_t global_index, const uint32_t bucket_id) noexcept { buckets_[bucket_id].push(global_index); }

    [[nodiscard]] const iovec* iovecs_ptr() const noexcept { return global_iovecs_.data(); }

    bool is_registered() const noexcept { return registered_; }
    void set_registered() noexcept { registered_ = true; }

    /// Query available slots in a specific bucket (debug/observability)
    [[nodiscard]] size_t available_in_bucket(const uint32_t bucket_id) const noexcept
    {
        if (bucket_id >= buckets_.size())
        {
            return 0;
        }
        return buckets_[bucket_id].free_stack.size();
    }

    /// Total number of buffer slots across all buckets
    [[nodiscard]] size_t total_capacity() const noexcept { return global_iovecs_.size(); }

    /// Number of buckets (for iteration/debugging)
    [[nodiscard]] size_t bucket_count() const noexcept { return buckets_.size(); }
};

}  // namespace URing