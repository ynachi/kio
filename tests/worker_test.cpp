//
// Created by Yao ACHI on 25/10/2025.
//

#include "core/include/io/worker.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "core/include/coro.h"
#include "core/include/errors.h"
#include "core/include/sync_wait.h"

using namespace kio;
using namespace kio::io;

class WorkerTest : public ::testing::Test
{
protected:
    std::unique_ptr<Worker> worker;
    std::unique_ptr<std::jthread> worker_thread;
    WorkerConfig config;

    void SetUp() override
    {
        // Suppress logging spam during tests
        // spdlog::set_level(spdlog::level::off);

        config.uring_submit_timeout_ms = 10;
        worker = std::make_unique<Worker>(0, config);

        // Start the worker's event loop on a dedicated thread
        worker_thread = std::make_unique<std::jthread>([this] { worker->loop_forever(); });

        // CRITICAL: Wait for the worker to be fully initialized
        // before running any test logic.
        worker->wait_ready();
    }

    void TearDown() override
    {
        // Request the worker to stop and wait for its thread to join.
        (void) worker->request_stop();
        worker_thread.reset();
        worker.reset();
    }

    // Helper coroutine to run a test on the worker thread
    template<typename Awaitable>
    auto RunOnWorker(Awaitable &&awaitable)
    {
        using AwaitableType = std::remove_reference_t<Awaitable>;
        auto task = [&]() -> Task<decltype(std::declval<AwaitableType>().await_resume())>
        {
            co_await SwitchToWorker(*worker);
            co_return co_await std::forward<Awaitable>(awaitable);
        };

        return SyncWait(task());
    }
};

TEST_F(WorkerTest, AsyncSleep)
{
    auto test_coro = [&]() -> Task<void>
    {
        // We must switch to the worker thread to use its io_uring features
        co_await SwitchToWorker(*worker);

        const auto start = std::chrono::steady_clock::now();
        auto result = co_await worker->async_sleep(std::chrono::milliseconds(20));
        const auto end = std::chrono::steady_clock::now();

        // Check 1: The operation must succeed
        EXPECT_TRUE(result.has_value());

        // Check 2: At least 20ms must have passed
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        EXPECT_GE(duration.count(), 20);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, AsyncReadOnBadFdReturnsError)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);
        constexpr int bad_fd = 999;
        char buffer[10];

        // We expect this to fail
        auto result = co_await worker->async_read(bad_fd, std::span(buffer));

        // Check 1: The operation must have failed
        EXPECT_FALSE(result.has_value());

        // Check 2: The error must be 'InvalidFileDescriptor' (EBADF)
        EXPECT_EQ(result.error().value, EBADF);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, AsyncReadWriteSocketPair)
{
    int fds[2];
    // Create a connected pair: fds[0] <--> fds[1]
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int read_fd = fds[0];
    const int write_fd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        // 1. Asynchronously write "hello" to one end
        std::string write_buf = "hello";
        auto write_res = co_await worker->async_write(write_fd, std::span(write_buf.data(), write_buf.size()));
        EXPECT_TRUE(write_res.has_value());
        EXPECT_EQ(*write_res, 5);

        // 2. Asynchronously read from the other end
        char read_buf[10]{};
        auto read_res = co_await worker->async_read(read_fd, std::span(read_buf, sizeof(read_buf)));
        EXPECT_TRUE(read_res.has_value());
        EXPECT_EQ(*read_res, 5);

        // 3. Verify the data
        EXPECT_EQ(std::string(read_buf, 5), "hello");
    };

    SyncWait(test_coro());

    // Clean up fds
    ::close(read_fd);
    ::close(write_fd);
}

TEST_F(WorkerTest, AsyncReadWriteExactSocketPair)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int read_fd = fds[0];
    const int write_fd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        // 1. Asynchronously write the *exact* buffer
        std::string write_buf = "hello world";
        auto write_res = co_await worker->async_write_exact(write_fd, std::span(write_buf.data(), write_buf.size()));

        // Check 1: The write_exact operation must succeed
        EXPECT_TRUE(write_res.has_value());

        // 2. Asynchronously read the *exact* buffer
        std::vector<char> read_buf(write_buf.size());
        auto read_res = co_await worker->async_read_exact(read_fd, std::span(read_buf.data(), read_buf.size()));

        // Check 2: The read_exact operation must succeed
        EXPECT_TRUE(read_res.has_value());

        // 3. Verify the data
        EXPECT_EQ(std::string(read_buf.data(), read_buf.size()), "hello world");
    };

    SyncWait(test_coro());

    // Clean up fds
    ::close(read_fd);
    ::close(write_fd);
}

TEST_F(WorkerTest, AsyncReadExactEOF)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int read_fd = fds[0];
    const int write_fd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        // 1. Write a *smaller* amount of data than the read will expect
        std::string write_buf = "short";
        const auto write_res = co_await worker->async_write(write_fd, std::span(write_buf.data(), write_buf.size()));
        EXPECT_TRUE(write_res.has_value());

        // 2. Close the writing end. This will send an EOF to the read end after
        //    the initial 5 bytes are read.
        //    We use async_close to ensure it happens on the worker thread.
        const auto close_res = co_await worker->async_close(write_fd);
        EXPECT_TRUE(close_res.has_value());
        // write_fd is now closed

        // 3. Attempt to read *exactly* 10 bytes. This should fail.
        char read_buf[10];  // Expect 10 bytes
        auto read_res = co_await worker->async_read_exact(read_fd, std::span(read_buf, sizeof(read_buf)));

        // Check 1: The operation must have failed
        EXPECT_FALSE(read_res.has_value());

        // Check 2: The error must be due to the broken pipe / EOF
        // This is the correct error our implementation returns (from EPIPE)
        EXPECT_EQ(read_res.error().value, kIoEof);
    };

    SyncWait(test_coro());

    ::close(read_fd);
}

TEST_F(WorkerTest, SingleThreadedExecution)
{
    // This test proves that a Worker is fully single-threaded by:
    // 1. Having multiple regular tasks modify a shared variable
    // 2. Having a detached task also modify the same variable
    // 3. No synchronization primitives are needed (no mutexes, atomics, etc.)
    // 4. All operations are sequential and deterministic

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        // Shared variable - NO synchronization needed because Worker is single-threaded
        int shared_counter = 0;
        bool detached_task_completed = false;

        // Launch a detached task that will increment the counter
        auto detached_increment = [&]() -> DetachedTask
        {
            co_await SwitchToWorker(*worker);

            // Even though this is "detached", it runs on the same worker thread
            // So it will execute cooperatively with other tasks
            for (int i = 0; i < 100; ++i)
            {
                shared_counter++;
                // Yield control to allow other tasks to run
                co_await worker->async_sleep(std::chrono::milliseconds(0));
            }
            detached_task_completed = true;
        };

        // Start the detached task
        detached_increment().detach();

        // Multiple regular tasks also incrementing the counter
        auto increment_task = [&](int iterations) -> Task<void>
        {
            co_await SwitchToWorker(*worker);

            for (int i = 0; i < iterations; ++i)
            {
                shared_counter++;
                // Yield to allow interleaving
                co_await worker->async_sleep(std::chrono::milliseconds(0));
            }
        };

        // Launch multiple tasks concurrently (but they execute sequentially on the worker)
        auto task1 = increment_task(50);
        auto task2 = increment_task(75);
        auto task3 = increment_task(25);

        // Wait for all regular tasks to complete
        co_await task1;
        co_await task2;
        co_await task3;

        // Wait for a detached task to complete by polling
        // (In real code, you'd use better coordination, but this demonstrates the concept)
        while (!detached_task_completed)
        {
            co_await worker->async_sleep(std::chrono::milliseconds(1));
        }

        // Check: All increments should have happened
        // 100 (detached) + 50 + 75 + 25 = 250
        EXPECT_EQ(shared_counter, 250);

        // The key insight: We didn't need ANY synchronization primitives!
        // No std::atomic, no std::mutex, no locks - because everything
        // runs sequentially on the same worker thread.
    };

    SyncWait(test_coro());
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
