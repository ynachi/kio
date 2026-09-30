#include "uring/core/buffer_pool.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace URing
{
FixedBuffer::FixedBuffer(FixedBuffer&& other) noexcept
    : index_(std::exchange(other.index_, kInvalidBufIndex)),
      bucket_id_(other.bucket_id_),
      view_(std::exchange(other.view_, {})),
      pool_(std::exchange(other.pool_, nullptr))
{
}

FixedBuffer& FixedBuffer::operator=(FixedBuffer&& other) noexcept
{
    if (this != &other) [[likely]]
    {
        release();
        index_ = std::exchange(other.index_, kInvalidBufIndex);
        bucket_id_ = other.bucket_id_;
        view_ = std::exchange(other.view_, {});
        pool_ = std::exchange(other.pool_, nullptr);
    }
    return *this;
}

void FixedBuffer::release() noexcept
{
    if (pool_ && index_ != kInvalidBufIndex)
    {
        pool_->release(index_, bucket_id_);
        index_ = kInvalidBufIndex;
        pool_ = nullptr;
        view_ = {};
    }
}

FixedBufferPool::Bucket::Bucket(const size_t size, const size_t count, const uint32_t start)
    : slot_size(size), start_index(start), slab(nullptr, std::free)
{
    // Allocate page-aligned slab (required for io_uring fixed buffers + O_DIRECT)
    void* ptr = nullptr;
    const size_t total = size * count;

    if (posix_memalign(&ptr, kBufferAlignment, total) != 0)
    {
        throw std::bad_alloc();
    }
    slab.reset(static_cast<std::byte*>(ptr));

    free_stack.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        free_stack.push_back(start + (count - 1 - i));
    }
}

[[nodiscard]] Result<uint32_t> FixedBufferPool::Bucket::pop() noexcept
{
    if (free_stack.empty()) [[unlikely]]
    {
        // resize is not allowed
        return std::unexpected(make_error_code(PoolError::Exhausted));
    }
    const uint32_t idx = free_stack.back();
    free_stack.pop_back();
    return idx;
}

FixedBufferPool::FixedBufferPool(std::initializer_list<BucketConfig> configs)
{
    if (configs.size() == 0)
    {
        return;
    }

    // Sort by slot_size ascending for efficient binary search in take()
    std::vector sorted_configs(configs);
    std::ranges::sort(sorted_configs, [](const auto& a, const auto& b) { return a.size < b.size; });

    // Fixed-buffer SQEs use a 16-bit index. Validate before allocating slabs.
    constexpr size_t max_slots = size_t{std::numeric_limits<uint16_t>::max()} + 1;
    size_t total_slots = 0;
    for (const auto& cfg : sorted_configs)
    {
        if (cfg.size == 0 || cfg.count == 0 || cfg.size % 4096 != 0)
        {
            throw std::invalid_argument("slot_size must be page-aligned (multiple of 4096) for io_uring fixed buffers");
        }
        if (cfg.count > max_slots - total_slots || cfg.size > std::numeric_limits<size_t>::max() / cfg.count)
            throw std::invalid_argument("fixed-buffer configuration exceeds size or index limits");
        total_slots += cfg.count;
    }

    // Build buckets + global iovec array
    uint32_t current_global_index = 0;
    buckets_.reserve(configs.size());
    global_iovecs_.reserve(total_slots);
    for (const auto& config : sorted_configs)
    {
        buckets_.emplace_back(config.size, config.count, current_global_index);

        // Register each slot in the flattened iovec array
        for (uint32_t i = 0; i < config.count; ++i)
        {
            global_iovecs_.push_back({.iov_base = &buckets_.back().slab[i * config.size], .iov_len = config.size});
        }
        current_global_index += config.count;
    }
}

[[nodiscard]] Result<FixedBuffer> FixedBufferPool::take(const size_t size) noexcept
{
    // Binary search for first bucket with slot_size >= requested size
    const auto it = std::ranges::lower_bound(buckets_, size, {}, &Bucket::slot_size);

    if (it == buckets_.end()) [[unlikely]]
    {
        return std::unexpected(make_error_code(PoolError::SizeTooLarge));
    }

    Result<uint32_t> idx_res = it->pop();
    if (!idx_res.has_value()) [[unlikely]]
    {
        return std::unexpected(idx_res.error());
    }

    const uint32_t bucket_id = static_cast<uint32_t>(it - buckets_.begin());
    const uint32_t global_idx = *idx_res;

    // Compute local offset within bucket's slab
    const uint32_t local_idx = global_idx - it->start_index;
    std::span view(&it->slab[local_idx * it->slot_size], it->slot_size);

    return FixedBuffer(global_idx, bucket_id, view, this);
}

FixedBufferPool::~FixedBufferPool()
{
    size_t available = 0;
    for (const auto& bucket : buckets_)
        available += bucket.free_stack.size();
    // A borrowed buffer keeps a pointer into this pool and must not outlive it.
    if (available != global_iovecs_.size())
        std::terminate();
}
}  // namespace URing
