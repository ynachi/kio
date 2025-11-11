//
// Created by Yao ACHI on 11/11/2025.
//

#ifndef KIO_BUFFER_POOL_H
#define KIO_BUFFER_POOL_H

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace kio
{
    class BufferPool;

    /**
     * @brief A movable, RAII-style wrapper for a buffer from a pool.
     *
     * This object is created by a BufferPool and automatically returns
     * its internal buffer to the pool when it is destroyed.
     * It is movable to allow it to live on a coroutine's stack.
     *
     * @note PooledBuffer is not thread-safe.
     */
    class PooledBuffer
    {
    public:
        // Non-copyable
        PooledBuffer(const PooledBuffer&) = delete;
        PooledBuffer& operator=(const PooledBuffer&) = delete;

        PooledBuffer(PooledBuffer&& other) noexcept;
        PooledBuffer& operator=(PooledBuffer&& other) noexcept;

        ~PooledBuffer();

        /**
         * @brief Get a span for the buffer, resizing it if needed.
         * @param required_size The minimum size needed.
         * @return A span pointing to the buffer's data.
         */
        std::span<char> span(size_t required_size);

        // Accessors
        char* data() { return buffer_.data(); }
        [[nodiscard]] size_t size() const { return buffer_.size(); }
        [[nodiscard]] size_t capacity() const { return buffer_.capacity(); }

    private:
        friend class BufferPool;

        PooledBuffer(BufferPool* pool, std::vector<char>&& buffer) : pool_(pool), buffer_(std::move(buffer)) {}

        BufferPool* pool_ = nullptr;
        std::vector<char> buffer_;
    };

    struct BufferPoolStats
    {
        // Acquisition stats
        size_t total_acquires{0};
        size_t cache_hits{0};
        size_t cache_misses{0};

        // Release stats
        size_t total_releases{0};
        size_t releases_pooled{0};
        size_t releases_dropped{0};

        // Per-bucket stats
        std::array<size_t, 4> bucket_sizes{};
        std::array<size_t, 4> bucket_acquires{};
        std::array<size_t, 4> bucket_releases{};

        // Memory stats
        size_t total_pooled_memory{0};

        [[nodiscard]] double hit_rate() const
        {
            if (total_acquires == 0) return 0.0;
            return static_cast<double>(cache_hits) / static_cast<double>(total_acquires);
        }

        void reset() { *this = BufferPoolStats{}; }
    };

    /**
     * @brief A single-threaded buffer pool with predefined buckets.
     *
     * This class is NOT thread-safe and is designed to be owned by a
     * single-threaded context, such as a kio::io::Worker.
     */
    class BufferPool
    {
    public:
        friend class PooledBuffer;

        BufferPool() = default;

        // Non-copyable, non-movable
        BufferPool(const BufferPool&) = delete;
        BufferPool& operator=(const BufferPool&) = delete;
        BufferPool(BufferPool&&) = delete;
        BufferPool& operator=(BufferPool&&) = delete;

        // Size classes for bucketing
        static constexpr size_t kSmallSize = 4 * 1024;  // 4 KB
        static constexpr size_t kMediumSize = 64 * 1024;  // 64 KB
        static constexpr size_t kLargeSize = 512 * 1024;  // 512 KB

        static constexpr size_t kMaxBuffersPerBucket = 20;

        /**
         * @brief Acquires a buffer from the pool.
         * @param required_size The minimum size needed for the buffer.
         * @return A PooledBuffer that manages the buffer's lifetime.
         */
        PooledBuffer acquire(size_t required_size);

        /**
         * @brief Returns a buffer to the pool.
         * @param buffer The buffer to return.
         */
        void release(std::vector<char>&& buffer);

        void clear()
        {
            for (auto& bucket: buckets_)
            {
                bucket.clear();
            }
            stats_.reset();
        }

        [[nodiscard]] const BufferPoolStats& stats() const { return stats_; }

    private:
        std::array<std::vector<std::vector<char>>, 4> buckets_;
        BufferPoolStats stats_;

        static size_t get_bucket_index(size_t size)
        {
            if (size <= kSmallSize) return 0;
            if (size <= kMediumSize) return 1;
            if (size <= kLargeSize) return 2;
            return 3;  // Extra-large
        }

        static size_t get_bucket_capacity(const size_t bucket_index)
        {
            static constexpr std::array<size_t, 4> capacities = {
                    kSmallSize, kMediumSize, kLargeSize, 0  // 0 means variable size
            };
            return capacities[bucket_index];
        }

        void update_stats_after_acquire(size_t bucket_idx, bool cache_hit);
        void update_stats_after_release(size_t bucket_idx, bool pooled);
    };

}  // namespace kio

#endif  // KIO_BUFFER_POOL_H
