//
// Created by Yao ACHI on 11/11/2025.
//

#include "core/include/ds/buffer_pool.h"

namespace kio
{

    PooledBuffer::PooledBuffer(PooledBuffer&& other) noexcept : pool_(other.pool_), buffer_(std::move(other.buffer_))
    {
        // Prevent other from releasing
        other.pool_ = nullptr;
    }

    PooledBuffer& PooledBuffer::operator=(PooledBuffer&& other) noexcept
    {
        if (this != &other)
        {
            // Release old buffer back to pool
            if (pool_)
            {
                pool_->release(std::move(buffer_));
            }

            pool_ = other.pool_;
            buffer_ = std::move(other.buffer_);

            // Prevent other from releasing
            other.pool_ = nullptr;
        }
        return *this;
    }

    PooledBuffer::~PooledBuffer()
    {
        if (pool_)
        {
            pool_->release(std::move(buffer_));
        }
    }

    std::span<char> PooledBuffer::span(const size_t required_size)
    {
        if (buffer_.capacity() < required_size)
        {
            buffer_.reserve(required_size);
        }
        buffer_.resize(required_size);
        return {buffer_.data(), required_size};
    }

    PooledBuffer BufferPool::acquire(const size_t required_size)
    {
        const size_t bucket_idx = get_bucket_index(required_size);

        // Try to reuse from pool
        if (auto& bucket = buckets_[bucket_idx]; !bucket.empty())
        {
            auto buffer = std::move(bucket.back());
            bucket.pop_back();
            update_stats_after_acquire(bucket_idx, true);
            return {this, std::move(buffer)};
        }

        // Cache miss: allocate new buffer
        update_stats_after_acquire(bucket_idx, false);

        if (bucket_idx == 3)
        {
            // Extra-large: allocate exactly what's needed (not pooled)
            return {this, std::vector<char>(required_size)};
        }

        // Allocate with bucket's standard capacity
        std::vector<char> new_buffer;
        new_buffer.reserve(get_bucket_capacity(bucket_idx));
        return {this, std::move(new_buffer)};
    }

    void BufferPool::release(std::vector<char>&& buffer)
    {
        const size_t capacity = buffer.capacity();
        const size_t bucket_idx = get_bucket_index(capacity);

        // Don't pool extra-large buffers or oversized buffers
        if (bucket_idx == 3)
        {
            update_stats_after_release(bucket_idx, false);
            return;  // Let buffer be destroyed
        }

        // Allow some tolerance for std::vector over-allocation
        if (const size_t expected_capacity = get_bucket_capacity(bucket_idx); capacity > expected_capacity * 2)
        {
            update_stats_after_release(bucket_idx, false);
            return;  // Too large, don't pool
        }

        if (auto& bucket = buckets_[bucket_idx]; bucket.size() < kMaxBuffersPerBucket)
        {
            // Keep capacity, clear data
            buffer.clear();
            bucket.push_back(std::move(buffer));
            update_stats_after_release(bucket_idx, true);
        }
        else
        {
            // Pool for this bucket is full, drop buffer
            update_stats_after_release(bucket_idx, false);
        }
    }

    void BufferPool::update_stats_after_acquire(const size_t bucket_idx, const bool cache_hit)
    {
        stats_.total_acquires++;
        stats_.bucket_acquires[bucket_idx]++;

        if (cache_hit)
        {
            stats_.cache_hits++;
            stats_.bucket_sizes[bucket_idx]--;
        }
        else
        {
            stats_.cache_misses++;
        }
    }

    void BufferPool::update_stats_after_release(const size_t bucket_idx, const bool pooled)
    {
        stats_.total_releases++;
        stats_.bucket_releases[bucket_idx]++;

        if (pooled)
        {
            stats_.releases_pooled++;
            stats_.bucket_sizes[bucket_idx]++;

            // Update total pooled memory
            stats_.total_pooled_memory = 0;
            for (size_t i = 0; i < 3; ++i)
            {
                stats_.total_pooled_memory += stats_.bucket_sizes[i] * get_bucket_capacity(i);
            }
        }
        else
        {
            stats_.releases_dropped++;
        }
    }
}  // namespace kio
