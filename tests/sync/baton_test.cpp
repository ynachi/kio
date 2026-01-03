#include "kio/sync/baton.h"

#include "kio/core/coro.h"
#include "kio/core/worker.h"
#include "kio/sync/sync_wait.h"

#include <chrono>
#include <latch>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using kio::SyncWait;
using kio::Task;
using kio::io::SwitchToWorker;
using kio::io::Worker;
using kio::io::WorkerConfig;
using kio::sync::AsyncBaton;

// Helper to start a worker in a background thread
class TestWorker
{
public:
    Worker worker;
    std::thread thread;

    explicit TestWorker(size_t id) : worker(id, WorkerConfig{})
    {
        // Use LoopForever so it sets thread_id_, initializes the ring,
        // and handles the stop token correctly.
        thread = std::thread([this]() { worker.LoopForever(); });

        // Wait for the worker to initialize (ring creation, thread_id assignment)
        worker.WaitReady();
    }

    ~TestWorker()
    {
        (void)worker.RequestStop();
        if (thread.joinable())
        {
            thread.join();
        }
    }
};

class AsyncBatonTest : public ::testing::Test
{
protected:
    void SetUp() override { kio::alog::Configure(16, kio::LogLevel::kError); }
};

// -----------------------------------------------------------------------------
// 1. Basic Signalling
// -----------------------------------------------------------------------------

TEST_F(AsyncBatonTest, PostBeforeWait_ResumesImmediately)
{
    TestWorker w(0);
    AsyncBaton baton;

    // 1. Signal first
    baton.Post();
    ASSERT_TRUE(baton.IsReady());

    // 2. Wait should complete immediately without blocking (Fast Path)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto task = [&](Worker& worker) -> Task<void>
    {
        co_await SwitchToWorker(worker);
        co_await baton.Wait(worker);
    }(w.worker);

    SyncWait(std::move(task));
}

TEST_F(AsyncBatonTest, WaitBeforePost_BlocksUntilSignaled)
{
    TestWorker w(0);
    AsyncBaton baton;

    std::atomic<bool> task_completed = false;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto task = [&](Worker& worker) -> Task<void>
    {
        co_await SwitchToWorker(worker);
        co_await baton.Wait(worker);
        task_completed = true;
    }(w.worker);

    // 1. Start the task (it will suspend on baton)
    std::thread poster(
        [&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            EXPECT_FALSE(task_completed);  // Should still be waiting
            baton.Post();
        });

    SyncWait(std::move(task));

    poster.join();
    ASSERT_TRUE(task_completed);
}

// -----------------------------------------------------------------------------
// 2. Thread Affinity & Architecture Compliance (CRITICAL)
// -----------------------------------------------------------------------------

TEST_F(AsyncBatonTest, ThreadAffinity_ResumesOnCorrectWorker)
{
    TestWorker w1(1);
    TestWorker w2(2);
    AsyncBaton baton;

    // Verify that tasks launched on W1 resume on W1, and W2 on W2.
    // This ensures io_uring SINGLE_ISSUER constraints are respected.

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto make_affinity_task = [&](Worker& target_worker) -> Task<void>
    {
        co_await SwitchToWorker(target_worker);

        // Pre-wait check
        EXPECT_TRUE(target_worker.IsOnWorkerThread());

        co_await baton.Wait(target_worker);

        // Post-wait check
        EXPECT_TRUE(target_worker.IsOnWorkerThread());
    };

    auto t1 = make_affinity_task(w1.worker);
    auto t2 = make_affinity_task(w2.worker);

    // Use a latch to wait for both tasks to complete in the main thread
    std::latch tasks_done(2);

    // Wrapper to run SyncWait in background threads
    std::thread runner1(
        [&]
        {
            SyncWait(std::move(t1));
            tasks_done.count_down();
        });
    std::thread runner2(
        [&]
        {
            SyncWait(std::move(t2));
            tasks_done.count_down();
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    baton.Post();

    tasks_done.wait();
    runner1.join();
    runner2.join();
}

// -----------------------------------------------------------------------------
// 3. Multi-Waiter / Broadcast
// -----------------------------------------------------------------------------

TEST_F(AsyncBatonTest, Broadcast_WakesAllWaiters)
{
    TestWorker w(0);
    AsyncBaton baton;
    constexpr int kNumTasks = 5;
    std::atomic<int> completion_count = 0;

    // Launch N threads, each submitting a task to the SAME worker.
    // They will all stack up on the Baton's linked list.
    std::vector<std::thread> threads;
    std::latch start_latch(kNumTasks);

    for (int i = 0; i < kNumTasks; ++i)
    {
        threads.emplace_back(
            [&]
            {
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                auto t = [&](Worker& worker) -> Task<void>
                {
                    co_await SwitchToWorker(worker);
                    start_latch.count_down();
                    co_await baton.Wait(worker);
                    completion_count++;
                }(w.worker);
                SyncWait(std::move(t));
            });
    }

    // Wait for all tasks to be likely suspended
    start_latch.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_EQ(completion_count, 0);

    baton.Post();

    for (auto& t : threads)
        t.join();

    EXPECT_EQ(completion_count, kNumTasks);
}

// -----------------------------------------------------------------------------
// 4. Reset & Reusability
// -----------------------------------------------------------------------------

TEST_F(AsyncBatonTest, Reset_AllowsReusingBaton)
{
    TestWorker w(0);
    AsyncBaton baton;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto task = [&](Worker& worker) -> Task<void>
    {
        co_await SwitchToWorker(worker);
        co_await baton.Wait(worker);
    }(w.worker);

    // 1. First usage
    baton.Post();
    SyncWait(std::move(task));
    EXPECT_TRUE(baton.IsReady());

    // 2. Reset
    baton.Reset();
    EXPECT_FALSE(baton.IsReady());

    // 3. Second usage verification
    std::atomic<bool> done_second = false;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto task2 = [&](Worker& worker) -> Task<void>
    {
        co_await SwitchToWorker(worker);
        co_await baton.Wait(worker);
        done_second = true;
    }(w.worker);

    std::thread poster(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            baton.Post();
        });

    SyncWait(std::move(task2));
    poster.join();

    EXPECT_TRUE(done_second);
}

// -----------------------------------------------------------------------------
// 5. Safety Asserts (Debug Mode Only)
// -----------------------------------------------------------------------------

#ifndef NDEBUG
TEST_F(AsyncBatonTest, DestructionWhileWaiting_TriggersAssert)
{
    // Verifies that destroying a baton while a coroutine is waiting crashes.
    // This protects against Use-After-Free bugs.
    ASSERT_DEATH(
        {
            TestWorker w(0);
            {
                AsyncBaton baton;

                // Launch a task that waits forever
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                auto task = [&](Worker& worker) -> Task<void>
                {
                    co_await SwitchToWorker(worker);
                    co_await baton.Wait(worker);
                }(w.worker);

                // Manually resume to get it suspended on the baton
                // We steal the handle to simulate a leaked/detached task
                auto h = task.h;
                task.h = nullptr;
                h.resume();

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                // Baton destructor called here -> Should Assert
            }
        },
        ".*");
}
#endif