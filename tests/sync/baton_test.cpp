//
// Created by Yao ACHI on 22/11/2025.
//

#include "core/include/sync/baton.h"

#include <gtest/gtest.h>

#include "core/include/io/worker.h"

using namespace kio;
using namespace kio::sync;
using namespace kio::io;

TEST(AsyncBatonTest, NotifyBeforeWait)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    AsyncBaton baton(worker);
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

    worker.request_stop();
}

TEST(AsyncBatonTest, WaitThenNotify)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    AsyncBaton baton(worker);
    std::atomic completed{false};

    // Start waiting
    auto task = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);
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

    worker.request_stop();
}
