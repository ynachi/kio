#include "uring/spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace URing;

TEST(SpscQueueTest, DequeuesInFifoOrderAcrossSegments)
{
    SpscQueue<int, 3> queue;

    for (int i = 0; i < 10; ++i)
    {
        ASSERT_TRUE(queue.emplace(i));
    }

    for (int i = 0; i < 10; ++i)
    {
        int value = -1;
        ASSERT_TRUE(queue.try_dequeue(value));
        EXPECT_EQ(value, i);
    }

    EXPECT_TRUE(queue.consumer_empty());
}

TEST(SpscQueueTest, SupportsMoveOnlyValues)
{
    SpscQueue<std::unique_ptr<int>, 2> queue;
    ASSERT_TRUE(queue.enqueue(std::make_unique<int>(42)));

    std::unique_ptr<int> value;
    ASSERT_TRUE(queue.try_dequeue(value));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 42);
    EXPECT_TRUE(queue.consumer_empty());
}

TEST(SpscQueueTest, DequeuesBatchWithLimit)
{
    SpscQueue<int, 4> queue;
    for (int i = 0; i < 9; ++i)
    {
        ASSERT_TRUE(queue.emplace(i));
    }

    std::vector<int> values;
    values.reserve(9);

    EXPECT_EQ(queue.try_dequeue_batch(std::back_inserter(values), 3), 3);
    EXPECT_EQ(queue.try_dequeue_batch(std::back_inserter(values), 10), 6);
    EXPECT_TRUE(queue.consumer_empty());

    ASSERT_EQ(values.size(), 9);
    for (int i = 0; i < 9; ++i)
    {
        EXPECT_EQ(values[static_cast<std::size_t>(i)], i);
    }
}

TEST(SpscQueueTest, DrainBatchRunsMoveOnlyFunctions)
{
    SpscQueue<std::move_only_function<void()>, 2> queue;
    int sum = 0;

    for (int i = 1; i <= 5; ++i)
    {
        ASSERT_TRUE(queue.emplace([&sum, i] { sum += i; }));
    }

    EXPECT_EQ(queue.drain_batch(3, [](std::move_only_function<void()> fn) { fn(); }), 3);
    EXPECT_EQ(sum, 6);

    EXPECT_EQ(queue.drain_batch(10, [](std::move_only_function<void()> fn) { fn(); }), 2);
    EXPECT_EQ(sum, 15);
    EXPECT_TRUE(queue.consumer_empty());
}

TEST(SpscQueueTest, DrainBatchRemovesItemBeforeCallback)
{
    SpscQueue<int, 2> queue;
    ASSERT_TRUE(queue.emplace(1));
    ASSERT_TRUE(queue.emplace(2));

    EXPECT_THROW(
        (void)queue.drain_batch(
            1,
            [](int)
            {
                throw std::runtime_error("boom");
            }),
        std::runtime_error);

    int value = 0;
    ASSERT_TRUE(queue.try_dequeue(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.consumer_empty());
}

namespace
{
struct LifetimeProbe
{
    static inline std::atomic<int> alive{0};

    int value = 0;

    explicit LifetimeProbe(const int v) : value(v) { alive.fetch_add(1, std::memory_order_relaxed); }

    LifetimeProbe(LifetimeProbe&& other) noexcept : value(std::exchange(other.value, -1))
    {
        alive.fetch_add(1, std::memory_order_relaxed);
    }

    LifetimeProbe& operator=(LifetimeProbe&&) = delete;
    LifetimeProbe(const LifetimeProbe&) = delete;
    LifetimeProbe& operator=(const LifetimeProbe&) = delete;

    ~LifetimeProbe() { alive.fetch_sub(1, std::memory_order_relaxed); }
};
}  // namespace

TEST(SpscQueueTest, DestructorDestroysQueuedValues)
{
    LifetimeProbe::alive.store(0, std::memory_order_relaxed);

    {
        SpscQueue<LifetimeProbe, 2> queue;
        ASSERT_TRUE(queue.emplace(1));
        ASSERT_TRUE(queue.emplace(2));
        ASSERT_TRUE(queue.emplace(3));
        EXPECT_EQ(LifetimeProbe::alive.load(std::memory_order_relaxed), 3);
    }

    EXPECT_EQ(LifetimeProbe::alive.load(std::memory_order_relaxed), 0);
}

TEST(SpscQueueTest, ConcurrentProducerConsumerPreservesOrder)
{
    constexpr std::uint64_t kItems = 200'000;

    SpscQueue<std::uint64_t, 64> queue;
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};

    std::thread producer(
        [&]
        {
            for (std::uint64_t i = 0; i < kItems; ++i)
            {
                if (!queue.emplace(i))
                {
                    ok.store(false, std::memory_order_relaxed);
                    break;
                }
            }
            done.store(true, std::memory_order_release);
        });

    std::uint64_t expected = 0;
    while (!done.load(std::memory_order_acquire) || !queue.consumer_empty())
    {
        const std::size_t drained = queue.drain_batch(128,
                                                      [&](const std::uint64_t value)
                                                      {
                                                          if (value != expected)
                                                          {
                                                              ok.store(false, std::memory_order_relaxed);
                                                          }
                                                          ++expected;
                                                      });

        if (drained == 0)
        {
            std::this_thread::yield();
        }
    }

    producer.join();

    EXPECT_TRUE(ok.load(std::memory_order_relaxed));
    EXPECT_EQ(expected, kItems);
}

TEST(SpscQueueTest, ConcurrentMoveOnlyFunctionDrain)
{
    constexpr std::uint64_t kItems = 100'000;

    SpscQueue<std::move_only_function<void()>, 32> queue;
    std::atomic<bool> done{false};
    std::atomic<bool> enqueue_ok{true};
    std::uint64_t sum = 0;

    std::thread producer(
        [&]
        {
            for (std::uint64_t i = 1; i <= kItems; ++i)
            {
                if (!queue.emplace([&sum, i] { sum += i; }))
                {
                    enqueue_ok.store(false, std::memory_order_relaxed);
                    break;
                }
            }
            done.store(true, std::memory_order_release);
        });

    while (!done.load(std::memory_order_acquire) || !queue.consumer_empty())
    {
        const std::size_t drained = queue.drain_batch(128, [](std::move_only_function<void()> fn) { fn(); });
        if (drained == 0)
        {
            std::this_thread::yield();
        }
    }

    producer.join();

    EXPECT_TRUE(enqueue_ok.load(std::memory_order_relaxed));
    EXPECT_EQ(sum, (kItems * (kItems + 1)) / 2);
}
