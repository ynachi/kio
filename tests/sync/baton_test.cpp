//
// Created by Yao ACHI on 22/11/2025.
//

#include "kio/sync/baton.h"

#include <gtest/gtest.h>

#include "kio/core/worker.h"

using namespace kio;
using namespace kio::sync;
using namespace kio::io;

class AsyncBatonTest : public ::testing::Test
{
protected:
    std::unique_ptr<Worker> worker;
    std::unique_ptr<std::jthread> worker_thread;

    void SetUp() override
    {
        // Create worker with default config
        worker = std::make_unique<Worker>(0, WorkerConfig{});

        // Start a worker thread
        worker_thread = std::make_unique<std::jthread>([this] { worker->LoopForever(); });

        // Wait for worker to be ready
        worker->WaitReady();
    }

    void TearDown() override
    {
        // Request stop and wait for shutdown
        (void) worker->RequestStop();
        worker->WaitShutdown();

        worker_thread.reset();
        worker.reset();
    }
};

TEST_F(AsyncBatonTest, NotifyBeforeWait)
{
    AsyncBaton baton;
    baton.Post();  // signal first

    bool completed = false;
    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.Wait(*worker);  // Should not suspend
        completed = true;
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed);
}

TEST_F(AsyncBatonTest, WaitThenNotify)
{
    AsyncBaton baton;
    std::atomic completed{false};

    // Start waiting
    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.Wait(*worker);
        completed.store(true);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(completed.load());

    // Notify from another thread
    baton.Post();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
}

TEST_F(AsyncBatonTest, MultipleWaitersAllResume)
{
    AsyncBaton baton;
    std::atomic<int> completed{0};

    auto waiter = [&](int id) -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.Wait(*worker);
        EXPECT_TRUE(worker->IsOnWorkerThread());
        completed.fetch_add(1);
    };

    waiter(1).detach();
    waiter(2).detach();
    waiter(3).detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(completed.load(), 0);

    baton.Post();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(completed.load(), 3);
}

TEST_F(AsyncBatonTest, ReadyCheck)
{
    AsyncBaton baton;
    EXPECT_FALSE(baton.IsReady());

    baton.Post();
    EXPECT_TRUE(baton.IsReady());

    baton.Reset();
    EXPECT_FALSE(baton.IsReady());
}

TEST_F(AsyncBatonTest, CrossThreadNotify)
{
    AsyncBaton baton;
    std::atomic completed{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.Wait(*worker);
        completed.store(true);
    };

    task().detach();

    // Notify from a different thread
    std::jthread notifier(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            baton.Post();
        });

    notifier.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
}

TEST_F(AsyncBatonTest, ResetAllowsReuse)
{
    AsyncBaton baton;
    std::atomic<int> phase{0};

    auto first = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.Wait(*worker);
        phase.store(1);
    };

    auto second = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.Wait(*worker);
        phase.store(2);
    };

    first().detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(phase.load(), 0);

    baton.Post();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(phase.load(), 1);

    baton.Reset();
    second().detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(phase.load(), 1);

    baton.Post();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(phase.load(), 2);
}
