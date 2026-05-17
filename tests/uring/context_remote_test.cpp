#include "uring/context.h"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

using namespace URing;

TEST(IoContextRemoteTest, SpawnOnRunsFactoryOnTargetWorker)
{
    IoOptions opts;
    opts.tick_timeout_ms = 1;

    IoContext ctx(2, opts);
    std::atomic ran{0};
    std::atomic observed_worker{-1};

    ASSERT_TRUE(ctx.start(
        [&]
        {
            auto* current = IoWorker::current_io();
            ASSERT_NE(current, nullptr);

            if (current->id() == 0)
            {
                ASSERT_TRUE(ctx.spawn_on(ctx.worker(1),
                                         [&]() -> DetachedTask
                                         {
                                             observed_worker.store(static_cast<int>(IoWorker::current_io()->id()),
                                                                   std::memory_order_relaxed);
                                             ran.fetch_add(1, std::memory_order_relaxed);
                                             ctx.stop();
                                             co_return;
                                         }));
            }
        }));

    ctx.join();

    EXPECT_EQ(ran.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(observed_worker.load(std::memory_order_relaxed), 1);
}

TEST(IoContextRemoteTest, AllWorkersCanDispatchToAnotherWorker)
{
    IoOptions opts;
    opts.tick_timeout_ms = 1;

    constexpr std::size_t kWorkers = 4;
    IoContext ctx(kWorkers, opts);
    std::array<std::atomic<int>, kWorkers> observed{};
    std::atomic<int> ran{0};

    for (auto& value : observed)
    {
        value.store(0, std::memory_order_relaxed);
    }

    ASSERT_TRUE(ctx.start(
        [&]
        {
            auto* current = IoWorker::current_io();
            ASSERT_NE(current, nullptr);

            const std::size_t source = current->id();
            const std::size_t target = (source + 1) % kWorkers;

            ASSERT_TRUE(ctx.spawn_on(ctx.worker(target),
                                     [&, target]() -> DetachedTask
                                     {
                                         observed[target].fetch_add(1, std::memory_order_relaxed);
                                         if (ran.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                                             static_cast<int>(kWorkers))
                                         {
                                             ctx.stop();
                                         }
                                         co_return;
                                     }));
        }));

    ctx.join();

    EXPECT_EQ(ran.load(std::memory_order_relaxed), static_cast<int>(kWorkers));
    for (const auto& value : observed)
    {
        EXPECT_EQ(value.load(std::memory_order_relaxed), 1);
    }
}
