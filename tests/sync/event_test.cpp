
//
// Created by Yao ACHI on 22/11/2025.
//

#include "core/include/sync/event.h"

#include <gtest/gtest.h>

#include "core/include/io/worker.h"

using namespace kio;
using namespace kio::sync;
using namespace kio::io;

TEST(AsyncEventTest, NotifyBeforeWait)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    auto event = std::make_unique<AsyncEvent>(worker);
    auto completed = std::make_shared<std::atomic<bool>>(false);

    auto task = [event_ptr = event.get(), completed]() -> DetachedTask
    {
        co_await event_ptr->wait();
        completed->store(true, std::memory_order_release);
    };

    event->notify();  // Notify first
    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed->load(std::memory_order_acquire));
    EXPECT_TRUE(event->is_set());

    worker.request_stop();
}

TEST(AsyncEventTest, WaitThenNotify)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    auto event = std::make_unique<AsyncEvent>(worker);
    auto completed = std::make_shared<std::atomic<bool>>(false);

    auto task = [&worker, event_ptr = event.get(), completed]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);
        co_await event_ptr->wait();
        completed->store(true, std::memory_order_release);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(completed->load(std::memory_order_acquire));
    EXPECT_FALSE(event->is_set());

    event->notify();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed->load(std::memory_order_acquire));
    EXPECT_TRUE(event->is_set());

    worker.request_stop();
}

TEST(AsyncEventTest, MultipleWaiters)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    auto event = std::make_unique<AsyncEvent>(worker);
    auto completed_count = std::make_shared<std::atomic<int>>(0);

    constexpr int num_waiters = 5;

    for (int i = 0; i < num_waiters; ++i)
    {
        auto task = [&worker, event_ptr = event.get(), completed_count]() -> DetachedTask
        {
            co_await SwitchToWorker(worker);
            co_await event_ptr->wait();
            completed_count->fetch_add(1, std::memory_order_release);
        };
        task().detach();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(completed_count->load(std::memory_order_acquire), 0);

    event->notify();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(completed_count->load(std::memory_order_acquire), num_waiters);

    worker.request_stop();
}

TEST(AsyncEventTest, ResetAndReuse)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    auto event = std::make_unique<AsyncEvent>(worker);

    event->notify();
    EXPECT_TRUE(event->is_set());

    event->reset();
    EXPECT_FALSE(event->is_set());

    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto task = [&worker, event_ptr = event.get(), completed]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);
        co_await event_ptr->wait();
        completed->store(true, std::memory_order_release);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(completed->load(std::memory_order_acquire));

    event->notify();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(completed->load(std::memory_order_acquire));
    EXPECT_TRUE(event->is_set());

    worker.request_stop();
}

TEST(AsyncEventTest, MultipleNotify)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    auto event = std::make_unique<AsyncEvent>(worker);
    auto completed_count = std::make_shared<std::atomic<int>>(0);

    auto task = [&worker, event_ptr = event.get(), completed_count]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);
        co_await event_ptr->wait();
        completed_count->fetch_add(1, std::memory_order_release);
    };

    task().detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Multiple notifies should be idempotent
    event->notify();
    event->notify();
    event->notify();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(completed_count->load(std::memory_order_acquire), 1);
    EXPECT_TRUE(event->is_set());

    worker.request_stop();
}

TEST(AsyncEventTest, ConcurrentWaitAndNotify)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    auto event = std::make_unique<AsyncEvent>(worker);
    auto completed_count = std::make_shared<std::atomic<int>>(0);

    constexpr int num_tasks = 10;

    for (int i = 0; i < num_tasks; ++i)
    {
        auto task = [&worker, event_ptr = event.get(), completed_count]() -> DetachedTask
        {
            co_await SwitchToWorker(worker);
            co_await event_ptr->wait();
            completed_count->fetch_add(1, std::memory_order_release);
        };
        task().detach();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::jthread notifier([event_ptr = event.get()] { event_ptr->notify(); });

    notifier.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(completed_count->load(std::memory_order_acquire), num_tasks);

    worker.request_stop();
}

TEST(AsyncEventTest, WaitAfterNotify)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    auto event = std::make_unique<AsyncEvent>(worker);
    event->notify();

    auto completed_count = std::make_shared<std::atomic<int>>(0);

    for (int i = 0; i < 3; ++i)
    {
        auto task = [&worker, event_ptr = event.get(), completed_count]() -> DetachedTask
        {
            co_await SwitchToWorker(worker);
            co_await event_ptr->wait();
            completed_count->fetch_add(1, std::memory_order_release);
        };
        task().detach();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(completed_count->load(std::memory_order_acquire), 3);

    worker.request_stop();
}

TEST(AsyncEventTest, StaggeredWaiters)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    auto event = std::make_unique<AsyncEvent>(worker);
    auto completed_count = std::make_shared<std::atomic<int>>(0);

    auto add_waiter = [&worker, event_ptr = event.get(), completed_count]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);
        co_await event_ptr->wait();
        completed_count->fetch_add(1, std::memory_order_release);
    };

    add_waiter().detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    add_waiter().detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    add_waiter().detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(completed_count->load(std::memory_order_acquire), 0);

    event->notify();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(completed_count->load(std::memory_order_acquire), 3);

    worker.request_stop();
}
