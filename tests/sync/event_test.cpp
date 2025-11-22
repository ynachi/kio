//
// Created by Yao ACHI on 22/11/2025.
//

#include "core/include/sync/event.h"

#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <vector>

#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"

using namespace kio;
using namespace kio::sync;
using namespace kio::io;

class AsyncEventTest : public ::testing::Test
{
protected:
    std::unique_ptr<Worker> worker;
    std::unique_ptr<std::jthread> worker_thread;
    WorkerConfig config;

    void SetUp() override
    {
        config.uring_submit_timeout_ms = 10;
        worker = std::make_unique<Worker>(0, config);
        worker_thread = std::make_unique<std::jthread>([this] { worker->loop_forever(); });
        worker->wait_ready();
    }

    void TearDown() override
    {
        (void) worker->request_stop();
        worker_thread.reset();
        worker.reset();
    }
};

TEST_F(AsyncEventTest, EventBroadcast)
{
    auto event = std::make_shared<AsyncEvent>(*worker);
    std::atomic<int> wake_count{0};
    constexpr int NUM_WAITERS = 5;
    std::latch latch{NUM_WAITERS};

    auto waiter_task = [event, &wake_count, &latch, this](int id) -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await event->wait();
        wake_count++;
        latch.count_down();
    };

    for (int i = 0; i < NUM_WAITERS; ++i)
    {
        waiter_task(i).detach();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(wake_count, 0);

    event->notify();

    latch.wait();
    EXPECT_EQ(wake_count, NUM_WAITERS);
}

TEST_F(AsyncEventTest, EventPreSet)
{
    auto event = std::make_shared<AsyncEvent>(*worker);
    event->notify();
    EXPECT_TRUE(event->is_set());

    std::latch latch{1};
    bool completed = false;

    auto waiter_task = [event, &completed, &latch, this]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await event->wait();
        completed = true;
        latch.count_down();
    };

    waiter_task().detach();
    latch.wait();
    EXPECT_TRUE(completed);
}

TEST_F(AsyncEventTest, CrossThreadNotification)
{
    auto event = std::make_shared<AsyncEvent>(*worker);
    std::latch latch{1};

    auto waiter = [event, &latch, this]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await event->wait();
        latch.count_down();
    };

    waiter().detach();

    std::thread notifier(
            [event]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                event->notify();
            });

    latch.wait();
    notifier.join();
}

TEST_F(AsyncEventTest, ConcurrentWaitAndNotify)
{
    auto event = std::make_shared<AsyncEvent>(*worker);
    std::atomic<int> completed_count{0};
    constexpr int num_tasks = 10;
    std::latch all_done{num_tasks};

    for (int i = 0; i < num_tasks; ++i)
    {
        auto task = [event, &completed_count, &all_done, this]() -> DetachedTask
        {
            co_await SwitchToWorker(*worker);
            co_await event->wait();
            completed_count.fetch_add(1, std::memory_order_release);
            all_done.count_down();
        };
        task().detach();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::jthread notifier([event] { event->notify(); });
    notifier.join();

    all_done.wait();

    EXPECT_EQ(completed_count.load(std::memory_order_acquire), num_tasks);
}

TEST_F(AsyncEventTest, WaitAfterNotify)
{
    auto event = std::make_shared<AsyncEvent>(*worker);
    event->notify();

    std::atomic<int> completed_count{0};
    std::latch latch{3};

    for (int i = 0; i < 3; ++i)
    {
        auto task = [event, &completed_count, &latch, this]() -> DetachedTask
        {
            co_await SwitchToWorker(*worker);
            co_await event->wait();
            completed_count.fetch_add(1, std::memory_order_release);
            latch.count_down();
        };
        task().detach();
    }

    latch.wait();
    EXPECT_EQ(completed_count.load(std::memory_order_acquire), 3);
}

TEST_F(AsyncEventTest, StaggeredWaiters)
{
    auto event = std::make_shared<AsyncEvent>(*worker);
    std::atomic<int> completed_count{0};
    std::latch latch{3};

    auto add_waiter = [event, &completed_count, &latch, this]() -> DetachedTask
    {
        co_await SwitchToWorker(*worker);
        co_await event->wait();
        completed_count.fetch_add(1, std::memory_order_release);
        latch.count_down();
    };

    add_waiter().detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    add_waiter().detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    add_waiter().detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(completed_count.load(std::memory_order_acquire), 0);

    event->notify();

    latch.wait();
    EXPECT_EQ(completed_count.load(std::memory_order_acquire), 3);
}
