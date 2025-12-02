//
// Created by Yao ACHI on 22/11/2025.
//

#include "kio/include/sync/baton.h"

#include <gtest/gtest.h>

#include "kio/include/io/worker.h"

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
        worker_thread = std::make_unique<std::jthread>([this] { worker->loop_forever(); });

        // Wait for worker to be ready
        worker->wait_ready();
    }

    void TearDown() override
    {
        // Request stop and wait for shutdown
        (void) worker->request_stop();
        worker->wait_shutdown();

        worker_thread.reset();
        worker.reset();
    }
};

TEST_F(AsyncBatonTest, NotifyBeforeWait)
{
    AsyncBaton baton(*worker);
    baton.notify();  // Notify first

    bool completed = false;
    auto task = [&]() -> DetachedTask
    {
        co_await baton.wait();  // Should not suspend
        completed = true;
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed);
}

TEST_F(AsyncBatonTest, WaitThenNotify)
{
    AsyncBaton baton(*worker);
    std::atomic completed{false};

    // Start waiting
    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.wait();
        completed.store(true);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(completed.load());

    // Notify from another thread
    baton.notify();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
}

TEST_F(AsyncBatonTest, WaitForTimeout)
{
    AsyncBaton baton(*worker);
    std::atomic completed{false};
    std::atomic timed_out{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        bool signaled = co_await baton.wait_for(std::chrono::milliseconds(100));
        timed_out.store(!signaled);
        completed.store(true);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(completed.load());

    // Wait for timeout to occur
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(timed_out.load());
}

TEST_F(AsyncBatonTest, WaitForSignaledBeforeTimeout)
{
    AsyncBaton baton(*worker);
    std::atomic completed{false};
    std::atomic signaled{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        const bool result = co_await baton.wait_for(std::chrono::milliseconds(500));
        signaled.store(result);
        completed.store(true);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(completed.load());

    // Notify before timeout
    baton.notify();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(signaled.load());
}

TEST_F(AsyncBatonTest, WaitForAlreadySignaled)
{
    AsyncBaton baton(*worker);
    baton.notify();  // Signal before wait_for

    std::atomic completed{false};
    std::atomic signaled{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        bool result = co_await baton.wait_for(std::chrono::milliseconds(100));
        signaled.store(result);
        completed.store(true);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(signaled.load());
}

TEST_F(AsyncBatonTest, WaitForRaceCondition)
{
    // Test the race where notify() happens during timer setup
    AsyncBaton baton(*worker);
    std::atomic completed{false};
    std::atomic signaled{false};
    std::atomic notify_called{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        const bool result = co_await baton.wait_for(std::chrono::milliseconds(200));
        signaled.store(result);
        completed.store(true);
    };

    task().detach();

    // Notify very quickly to create race condition
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    baton.notify();
    notify_called.store(true);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(notify_called.load());
    EXPECT_TRUE(signaled.load());
}

TEST_F(AsyncBatonTest, ResetAndReuse)
{
    AsyncBaton baton(*worker);
    std::atomic iteration{0};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);

        // First use
        co_await baton.wait();
        iteration.store(1);
        baton.reset();

        // Second use
        bool result = co_await baton.wait_for(std::chrono::milliseconds(100));
        if (result)
        {
            iteration.store(2);
        }
        else
        {
            iteration.store(3);  // timeout
        }
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(iteration.load(), 0);

    // First notify
    baton.notify();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(iteration.load(), 1);

    // Second notify before timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    baton.notify();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(iteration.load(), 2);
}

TEST_F(AsyncBatonTest, MultipleNotifyIdempotent)
{
    AsyncBaton baton(*worker);
    std::atomic completed{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.wait();
        completed.store(true);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(completed.load());

    // Multiple notifies should be safe
    baton.notify();
    baton.notify();
    baton.notify();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
}

TEST_F(AsyncBatonTest, WaitForZeroTimeout)
{
    AsyncBaton baton(*worker);
    std::atomic completed{false};
    std::atomic timed_out{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        bool result = co_await baton.wait_for(std::chrono::milliseconds(0));
        timed_out.store(!result);
        completed.store(true);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(timed_out.load());
}

TEST_F(AsyncBatonTest, WaitForConcurrentNotifyDuringTimeout)
{
    // Test edge case: notify happens right as timeout is expiring
    AsyncBaton baton(*worker);
    std::atomic completed{false};
    std::atomic result_recorded{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        bool result = co_await baton.wait_for(std::chrono::milliseconds(100));
        result_recorded.store(result);
        completed.store(true);
    };

    task().detach();

    // Wait until just before timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(95));

    // Notify at the last moment
    baton.notify();

    // Give time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(completed.load());
    // Result could be either true (notify won) or false (timeout won)
    // Both are acceptable depending on the exact timing
}

TEST_F(AsyncBatonTest, ReadyCheck)
{
    AsyncBaton baton(*worker);
    EXPECT_FALSE(baton.ready());

    baton.notify();
    EXPECT_TRUE(baton.ready());

    baton.reset();
    EXPECT_FALSE(baton.ready());
}

TEST_F(AsyncBatonTest, CrossThreadNotify)
{
    AsyncBaton baton(*worker);
    std::atomic completed{false};

    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await baton.wait();
        completed.store(true);
    };

    task().detach();

    // Notify from a different thread
    std::jthread notifier(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                baton.notify();
            });

    notifier.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed.load());
}
