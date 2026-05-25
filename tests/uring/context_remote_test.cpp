#include "../../include/uring/core/io.h"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

using namespace URing;

TEST(IoContextRemoteTest, ScheduleRunsOnTargetWorker)
{
    IoOptions opts;
    opts.tick_timeout_ms = 1;

    IoContext ctx(2, opts);
    std::atomic<int> ran{0};
    std::atomic<int> observed_worker{-1};

    // Define a task that records the ID of the worker it runs on
    auto task = [&](IO& target) -> Task<void> {
        observed_worker.store(static_cast<int>(target.id()), std::memory_order_relaxed);
        ran.fetch_add(1, std::memory_order_relaxed);
        co_return {};
    };

    // Schedule on worker 1 from the main thread
    ctx.worker(1).schedule(task(ctx.worker(1)));

    // Busy wait for completion with timeout
    auto start = std::chrono::steady_clock::now();
    while (ran.load() == 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(2))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ctx.join();

    EXPECT_EQ(ran.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(observed_worker.load(std::memory_order_relaxed), 1);
}

TEST(IoContextRemoteTest, WorkersCanScheduleOnAnotherWorker)
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

    // A task that schedules another task on a different worker
    auto hopping_task = [&](IO& source, IO& target) -> Task<void> {
        auto target_task = [&](IO& t) -> Task<void> {
            observed[t.id()].fetch_add(1, std::memory_order_relaxed);
            ran.fetch_add(1, std::memory_order_acq_rel);
            co_return {};
        };
        target.schedule(target_task(target));
        co_return {};
    };

    // Chain scheduling: 0 -> 1, 1 -> 2, 2 -> 3, 3 -> 0
    for (std::size_t i = 0; i < kWorkers; ++i)
    {
        std::size_t next = (i + 1) % kWorkers;
        ctx.worker(i).schedule(hopping_task(ctx.worker(i), ctx.worker(next)));
    }

    // Wait for all tasks to complete
    auto start = std::chrono::steady_clock::now();
    while (ran.load() < static_cast<int>(kWorkers) && std::chrono::steady_clock::now() - start < std::chrono::seconds(2))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ctx.join();

    EXPECT_EQ(ran.load(std::memory_order_relaxed), static_cast<int>(kWorkers));
    for (const auto& value : observed)
    {
        EXPECT_EQ(value.load(std::memory_order_relaxed), 1);
    }
}
