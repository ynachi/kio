//
// Created by Yao ACHI on 25/10/2025.
//

#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"
#include "core/include/errors.h"
#include "core/include/coro.h"

using namespace kio;
using namespace kio::io;

class WorkerTest : public ::testing::Test {
protected:
    std::unique_ptr<Worker> worker;
    std::unique_ptr<std::jthread> worker_thread;
    WorkerConfig config;

    void SetUp() override {
        // Suppress logging spam during tests
        // spdlog::set_level(spdlog::level::off);

        config.uring_submit_timeout_ms = 10;
        worker = std::make_unique<Worker>(0, config);

        // Start the worker's event loop on a dedicated thread
        worker_thread = std::make_unique<std::jthread>([this] {
            worker->loop_forever();
        });

        // CRITICAL: Wait for the worker to be fully initialized
        // before running any test logic.
        worker->wait_ready();
    }

    void TearDown() override {
        // Request the worker to stop and wait for its thread to join.
        (void) worker->request_stop();
        worker_thread.reset();
        worker.reset();
    }

    // Helper coroutine to run a test on the worker thread
    template<typename Awaitable>
    auto RunOnWorker(Awaitable &&awaitable) {
        using AwaitableType = std::remove_reference_t<Awaitable>;
        auto task = [&]() -> Task<decltype(std::declval<AwaitableType>().await_resume())> {
            co_await SwitchToWorker(*worker);
            co_return co_await std::forward<Awaitable>(awaitable);
        };

        return SyncWait(task());
    }
};

// Test Case 1: Test the async_sleep functionality
TEST_F(WorkerTest, AsyncSleep) {
    auto test_coro = [&]() -> Task<void> {
        // We must switch to the worker thread to use its io_uring features
        co_await SwitchToWorker(*worker);

        const auto start = std::chrono::steady_clock::now();
        auto result = co_await worker->async_sleep(std::chrono::milliseconds(20));
        const auto end = std::chrono::steady_clock::now();

        // Check 1: The operation must succeed
        EXPECT_TRUE(result.has_value()) << result.error().message();

        // Check 2: At least 20ms must have passed
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        EXPECT_GE(duration.count(), 20);
    };

    SyncWait(test_coro());
}

// Test Case 2: Test that error paths are handled correctly
TEST_F(WorkerTest, AsyncReadOnBadFdReturnsError) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker);
        constexpr int bad_fd = 999;
        char buffer[10];

        // We expect this to fail
        auto result = co_await worker->async_read(bad_fd, std::span(buffer), 0);

        // Check 1: The operation must have failed
        EXPECT_FALSE(result.has_value());

        // Check 2: The error must be 'InvalidFileDescriptor' (EBADF)
        EXPECT_EQ(result.error().category, IoError::InvalidFileDescriptor);
    };

    SyncWait(test_coro());
}

// Test Case 3: Test read/write using a connected socket pair (in-memory)
TEST_F(WorkerTest, AsyncReadWriteSocketPair) {
    int fds[2];
    // Create a connected pair: fds[0] <--> fds[1]
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int read_fd = fds[0];
    const int write_fd = fds[1];

    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker);

        // 1. Asynchronously write "hello" to one end
        std::string write_buf = "hello";
        auto write_res = co_await worker->async_write(write_fd, std::span(write_buf.data(), write_buf.size()), 0);
        EXPECT_TRUE(write_res.has_value());
        EXPECT_EQ(*write_res, 5);

        // 2. Asynchronously read from the other end
        char read_buf[10]{};
        auto read_res = co_await worker->async_read(read_fd, std::span(read_buf, sizeof(read_buf)), 0);
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

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
