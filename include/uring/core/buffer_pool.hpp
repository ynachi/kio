#pragma once

#include <initializer_list>
#include <memory>
#include <span>
#include <vector>

#include <liburing.h>

#include "uring/error.hpp"

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
    friend class IO;
    friend class FixedBufferPool;
    uint32_t index_ = kInvalidBufIndex;  ///< Global index in io_uring iovec array
    uint32_t bucket_id_ = 0;             ///< Which bucket this buffer belongs to
    std::span<std::byte> view_;          ///< View into the pinned memory region
    FixedBufferPool* pool_ = nullptr;

    FixedBuffer(const uint32_t index, const uint32_t bucket_id, std::span<std::byte> view,
                FixedBufferPool* pool) noexcept
        : index_(index), bucket_id_(bucket_id), view_(view), pool_(pool)
    {
    }

public:
    FixedBuffer(const FixedBuffer&) = delete;

    FixedBuffer(FixedBuffer&& other) noexcept;

    FixedBuffer& operator=(FixedBuffer&& other) noexcept;

    ~FixedBuffer() noexcept { release(); }

    /// Return buffer to pool (idempotent)
    void release() noexcept;

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
    friend class IO;
    friend class FixedBuffer;
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

public:
    /// Construct pool from bucket configurations
    /// @param configs List of {slot_size, count} pairs (will be sorted internally)
    FixedBufferPool(std::initializer_list<BucketConfig> configs);
    ~FixedBufferPool();
    FixedBufferPool(const FixedBufferPool&) = delete;
    FixedBufferPool& operator=(const FixedBufferPool&) = delete;
    FixedBufferPool(FixedBufferPool&&) = delete;
    FixedBufferPool& operator=(FixedBufferPool&&) = delete;

    /// Acquire the smallest buffer >= requested size
    /// @param size Minimum buffer size needed
    /// @return FixedBuffer on success, PoolError on failure
    [[nodiscard]] Result<FixedBuffer> take(size_t size) noexcept;

private:
    void release(uint32_t global_index, const uint32_t bucket_id) noexcept { buckets_[bucket_id].push(global_index); }

    [[nodiscard]] const iovec* iovecs_ptr() const noexcept { return global_iovecs_.data(); }

    /// Total number of buffer slots across all buckets
    [[nodiscard]] size_t total_capacity() const noexcept { return global_iovecs_.size(); }
};

}  // namespace URing
