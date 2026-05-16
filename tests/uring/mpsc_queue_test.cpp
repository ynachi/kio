#include "uring/mpsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace URing;

namespace
{

struct IntNode : MpscNode
{
    explicit IntNode(int value_) : value(value_) {}

    int value;
};

}  // namespace

TEST(MpscQueueTest, DrainsSingleProducerInFifoOrder)
{
    MpscQueue queue;
    std::vector<std::unique_ptr<IntNode>> nodes;
    std::vector<int> values;

    for (int i = 0; i < 16; ++i)
    {
        nodes.push_back(std::make_unique<IntNode>(i));
        queue.push(nodes.back().get());
    }

    const std::size_t drained = queue.drain(
        [&](MpscNode* raw)
        {
            values.push_back(static_cast<IntNode*>(raw)->value);
        });

    EXPECT_EQ(drained, nodes.size());
    EXPECT_EQ(values.size(), nodes.size());
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_EQ(values[static_cast<std::size_t>(i)], i);
    }
    EXPECT_EQ(queue.try_pop(), nullptr);
}

TEST(MpscQueueTest, ReturnedSingletonNodesCanBeDeletedImmediately)
{
    MpscQueue queue;

    for (int i = 0; i < 1024; ++i)
    {
        auto* node = new IntNode(i);
        queue.push(node);

        MpscNode* raw = queue.try_pop();
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(static_cast<IntNode*>(raw)->value, i);
        delete static_cast<IntNode*>(raw);

        EXPECT_EQ(queue.try_pop(), nullptr);
    }
}

TEST(MpscQueueTest, DrainsManyConcurrentProducers)
{
    MpscQueue queue;
    constexpr int kProducerCount = 4;
    constexpr int kItemsPerProducer = 4096;
    constexpr int kTotalItems = kProducerCount * kItemsPerProducer;

    std::atomic<int> producers_left{kProducerCount};
    std::atomic<int> drained{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);

    for (int producer = 0; producer < kProducerCount; ++producer)
    {
        producers.emplace_back(
            [&queue, &producers_left, producer]
            {
                for (int i = 0; i < kItemsPerProducer; ++i)
                {
                    queue.push(new IntNode(producer * kItemsPerProducer + i));
                }
                producers_left.fetch_sub(1, std::memory_order_release);
            });
    }

    std::vector<int> values;
    values.reserve(kTotalItems);

    while (producers_left.load(std::memory_order_acquire) > 0 || drained.load(std::memory_order_relaxed) < kTotalItems)
    {
        const std::size_t count = queue.drain(
            [&](MpscNode* raw)
            {
                auto* node = static_cast<IntNode*>(raw);
                values.push_back(node->value);
                delete node;
            },
            128);

        drained.fetch_add(static_cast<int>(count), std::memory_order_relaxed);
        if (count == 0)
        {
            std::this_thread::yield();
        }
    }

    for (auto& producer : producers)
    {
        producer.join();
    }

    std::ranges::sort(values);

    ASSERT_EQ(values.size(), static_cast<std::size_t>(kTotalItems));
    for (int i = 0; i < kTotalItems; ++i)
    {
        EXPECT_EQ(values[static_cast<std::size_t>(i)], i);
    }
    EXPECT_EQ(queue.try_pop(), nullptr);
}
