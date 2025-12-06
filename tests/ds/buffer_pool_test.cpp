//
// Created by Yao ACHI on 11/11/2025.
//

#include "kio/include/ds/buffer_pool.h"

#include <gtest/gtest.h>

#include "../../kio/core/coro.h"
#include "../../kio/core/sync_wait.h"

using namespace kio;

class BufferPoolTest : public ::testing::Test
{
protected:
    BufferPool pool_;

    void SetUp() override { pool_.clear(); }
};

// ============================================================================
// Basic Functionality Tests
// ============================================================================

TEST_F(BufferPoolTest, AcquireSmallBuffer)
{
    auto buffer = pool_.acquire(1024);

    EXPECT_NE(buffer.data(), nullptr);
    EXPECT_GE(buffer.capacity(), 1024);
    EXPECT_EQ(buffer.size(), 0);  // Not resized yet
}

TEST_F(BufferPoolTest, AcquireMediumBuffer)
{
    auto buffer = pool_.acquire(32 * 1024);

    EXPECT_NE(buffer.data(), nullptr);
    EXPECT_GE(buffer.capacity(), 32 * 1024);
}

TEST_F(BufferPoolTest, AcquireLargeBuffer)
{
    auto buffer = pool_.acquire(256 * 1024);

    EXPECT_NE(buffer.data(), nullptr);
    EXPECT_GE(buffer.capacity(), 256 * 1024);
}

TEST_F(BufferPoolTest, AcquireExtraLargeBuffer)
{
    auto buffer = pool_.acquire(1024 * 1024);  // 1 MB

    EXPECT_NE(buffer.data(), nullptr);
    EXPECT_GE(buffer.capacity(), 1024 * 1024);
}

TEST_F(BufferPoolTest, BufferSpanResize)
{
    auto buffer = pool_.acquire(1024);

    auto span1 = buffer.span(512);
    EXPECT_EQ(span1.size(), 512);
    EXPECT_EQ(buffer.size(), 512);

    auto span2 = buffer.span(2048);
    EXPECT_EQ(span2.size(), 2048);
    EXPECT_EQ(buffer.size(), 2048);
}

// ============================================================================
// RAII and Move Semantics Tests
// ============================================================================

TEST_F(BufferPoolTest, BufferAutoReturnsToPool)
{
    {
        auto buffer = pool_.acquire(1024);
        // Buffer should be returned to pool when it goes out of scope
    }

    const auto& stats = pool_.stats();
    EXPECT_EQ(stats.total_releases, 1);
    EXPECT_EQ(stats.releases_pooled, 1);
}

TEST_F(BufferPoolTest, BufferMoveConstructor)
{
    auto buffer1 = pool_.acquire(1024);
    auto* ptr1 = buffer1.data();

    auto buffer2 = std::move(buffer1);

    EXPECT_EQ(buffer2.data(), ptr1);
    EXPECT_EQ(buffer1.data(), nullptr);  // Moved-from buffer is empty
}

TEST_F(BufferPoolTest, BufferMoveAssignment)
{
    auto buffer1 = pool_.acquire(1024);
    auto buffer2 = pool_.acquire(2048);

    auto* ptr1 = buffer1.data();

    buffer2 = std::move(buffer1);

    EXPECT_EQ(buffer2.data(), ptr1);
    EXPECT_EQ(buffer1.data(), nullptr);

    // The first buffer should have been released when overwritten
    const auto& stats = pool_.stats();
    EXPECT_EQ(stats.total_releases, 1);
}

// ============================================================================
// Pooling and Reuse Tests
// ============================================================================

TEST_F(BufferPoolTest, BufferReuseFromPool)
{
    // Acquire and release
    {
        auto buffer = pool_.acquire(1024);
    }

    const auto& stats_after_release = pool_.stats();
    EXPECT_EQ(stats_after_release.bucket_sizes[0], 1);  // Small bucket

    // Acquire again - should reuse
    auto buffer2 = pool_.acquire(1024);
    const auto& stats_after_reacquire = pool_.stats();

    EXPECT_EQ(stats_after_reacquire.cache_hits, 1);
    EXPECT_EQ(stats_after_reacquire.cache_misses, 1);  // First acquire was a miss
    EXPECT_EQ(stats_after_reacquire.bucket_sizes[0], 0);  // Pool is empty again
}

TEST_F(BufferPoolTest, MultipleBuffersInPool)
{
    std::vector<PooledBuffer> buffers;
    buffers.reserve(5);

    // Acquire 5 buffers and keep them alive
    for (int i = 0; i < 5; ++i)
    {
        buffers.push_back(pool_.acquire(1024));
    }

    // Now, release them all by clearing the vector
    buffers.clear();

    // Check stats AFTER release
    {
        const auto& stats = pool_.stats();
        EXPECT_EQ(stats.bucket_sizes[0], 5);
        EXPECT_EQ(stats.releases_pooled, 5);
        EXPECT_EQ(stats.cache_misses, 5);  // All 5 should have been misses
    }

    // All subsequent acquires should be cache hits
    for (int i = 0; i < 5; ++i)
    {
        buffers.push_back(pool_.acquire(1024));
    }

    // Check stats AFTER re-acquiring
    const auto& stats_after_reacquire = pool_.stats();
    EXPECT_EQ(stats_after_reacquire.cache_hits, 5);
    EXPECT_EQ(stats_after_reacquire.bucket_sizes[0], 0);  // The pool is empty again
}

TEST_F(BufferPoolTest, PoolCapacityLimit)
{
    std::vector<PooledBuffer> buffers;
    constexpr size_t count = BufferPool::kMaxBuffersPerBucket + 5;
    buffers.reserve(count);

    // Acquire and keep alive
    for (size_t i = 0; i < count; ++i)
    {
        buffers.push_back(pool_.acquire(1024));
    }

    // Release all
    buffers.clear();

    const auto& stats = pool_.stats();

    // Only kMaxBuffersPerBucket should be pooled
    EXPECT_EQ(stats.bucket_sizes[0], BufferPool::kMaxBuffersPerBucket);
    EXPECT_EQ(stats.releases_pooled, BufferPool::kMaxBuffersPerBucket);
    EXPECT_EQ(stats.releases_dropped, 5);
}

// ============================================================================
// Size Class Bucketing Tests
// ============================================================================

TEST_F(BufferPoolTest, SmallBucketBoundary)
{
    auto buffer1 = pool_.acquire(BufferPool::kSmallSize);
    auto buffer2 = pool_.acquire(BufferPool::kSmallSize + 1);

    // Should release both, then check bucket assignments
    buffer1 = pool_.acquire(0);
    buffer2 = pool_.acquire(0);

    const auto& stats = pool_.stats();

    // First should be in a small bucket (idx 0)
    // Second should be in a medium bucket (idx 1)
    EXPECT_EQ(stats.bucket_releases[0], 1);
    EXPECT_EQ(stats.bucket_releases[1], 1);
}

TEST_F(BufferPoolTest, ExtraLargeNotPooled)
{
    {
        auto buffer = pool_.acquire(1024 * 1024);  // 1 MB
    }

    const auto& stats = pool_.stats();

    // Extra-large buffers should not be pooled
    EXPECT_EQ(stats.bucket_sizes[3], 0);
    EXPECT_EQ(stats.releases_dropped, 1);
}

// ============================================================================
// Coroutine Integration Test
// ============================================================================

TEST_F(BufferPoolTest, CoroutineUsage)
{
    auto coro = [&]() -> Task<size_t>
    {
        // Buffer lives on coroutine stack
        auto buffer = pool_.acquire(1024);

        auto span = buffer.span(512);

        // Simulate some work
        std::fill(span.begin(), span.end(), 'A');

        co_return span.size();
    };

    const size_t result = SyncWait(coro());
    EXPECT_EQ(result, 512);

    // Buffer should have been returned to pool
    const auto& stats = pool_.stats();
    EXPECT_EQ(stats.total_releases, 1);
    EXPECT_EQ(stats.releases_pooled, 1);
}

TEST_F(BufferPoolTest, NestedCoroutines)
{
    auto inner_coro = [&](size_t size) -> Task<size_t>
    {
        auto buffer = pool_.acquire(size);
        auto span = buffer.span(size);
        co_return span.size();
    };

    auto outer_coro = [&]() -> Task<size_t>
    {
        const size_t size1 = co_await inner_coro(1024);
        const size_t size2 = co_await inner_coro(2048);
        co_return size1 + size2;
    };

    const size_t result = SyncWait(outer_coro());
    EXPECT_EQ(result, 3072);

    // Both buffers should have been released
    const auto& stats = pool_.stats();
    EXPECT_EQ(stats.total_releases, 2);
}

TEST_F(BufferPoolTest, ZeroSizeAcquire)
{
    auto buffer = pool_.acquire(0);

    EXPECT_NE(buffer.data(), nullptr);
    // Should allocate smallest bucket
    EXPECT_GE(buffer.capacity(), BufferPool::kSmallSize);
}

TEST_F(BufferPoolTest, MultipleMovesInChain)
{
    auto buffer1 = pool_.acquire(1024);
    auto buffer2 = std::move(buffer1);
    auto buffer3 = std::move(buffer2);
    auto buffer4 = std::move(buffer3);

    // Only one release when buffer4 goes out of scope
    EXPECT_EQ(buffer4.capacity(), BufferPool::kSmallSize);
}

TEST_F(BufferPoolTest, ClearPool)
{
    // Populate pool
    for (int i = 0; i < 10; ++i)
    {
        auto buffer = pool_.acquire(1024);
    }

    pool_.clear();

    const auto& stats = pool_.stats();
    EXPECT_EQ(stats.bucket_sizes[0], 0);
    EXPECT_EQ(stats.total_pooled_memory, 0);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
