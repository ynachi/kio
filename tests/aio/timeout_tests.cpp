// tests/aio/timeout_tests.cpp
// Tests for timeout functionality

#include <gtest/gtest.h>

#include <array>
#include <chrono>

#include "aio/io_context.hpp"
#include "aio/io.hpp"
#include "test_helpers.hpp"

using namespace aio;
using namespace aio::test;
using namespace std::chrono_literals;

class TimeoutTest : public ::testing::Test {
protected:
    IoContext ctx{256};
};

// -----------------------------------------------------------------------------
// WithTimeout Tests
// -----------------------------------------------------------------------------

TEST_F(TimeoutTest, OperationTimesOut) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 32> buf{};

        // Recv with timeout - nothing sent, should timeout
        auto result = co_await AsyncRecv(ctx, sockets.fd2.Get(), buf, 0)
            .WithTimeout(50ms);

        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), std::make_error_code(std::errc::timed_out));
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(TimeoutTest, OperationCompletesBeforeTimeout) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    // Write data so recv succeeds immediately
    [[maybe_unused]] auto w = ::write(sockets.fd1.Get(), "data", 4);

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 32> buf{};

        auto result = co_await AsyncRecv(ctx, sockets.fd2.Get(), buf, 0)
            .WithTimeout(5s);  // Long timeout

        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(*result, 4u);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(TimeoutTest, SleepWithTimeout) {
    auto test = [&]() -> Task<void> {
        // Sleep that completes before timeout
        auto result = co_await AsyncSleep(ctx, 10ms).WithTimeout(1s);
        EXPECT_TRUE(result.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(TimeoutTest, WriteWithTimeout) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        auto data = AsBytes("test data");

        // Write should complete quickly
        auto result = co_await AsyncWrite(ctx, file.Get(), data, 0)
            .WithTimeout(1s);

        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(*result, 9u);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(TimeoutTest, ReadWithTimeoutSuccess) {
    auto file = MakeTempFileWithContent("content");
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 16> buf{};

        auto result = co_await AsyncRead(ctx, file.Get(), buf, 0)
            .WithTimeout(1s);

        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(*result, 7u);  // "content"
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(TimeoutTest, TimeoutAccuracyLowerBound) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 32> buf{};
        auto start = std::chrono::steady_clock::now();

        auto result = co_await AsyncRecv(ctx, sockets.fd2.Get(), buf, 0)
            .WithTimeout(100ms);

        auto elapsed = std::chrono::steady_clock::now() - start;

        EXPECT_FALSE(result.has_value());
        // Should take at least ~100ms
        EXPECT_GE(elapsed, 90ms);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(TimeoutTest, TimeoutAccuracyUpperBound) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 32> buf{};
        auto start = std::chrono::steady_clock::now();

        auto result = co_await AsyncRecv(ctx, sockets.fd2.Get(), buf, 0)
            .WithTimeout(100ms);

        auto elapsed = std::chrono::steady_clock::now() - start;

        EXPECT_FALSE(result.has_value());
        // Should not take much longer than 100ms
        EXPECT_LE(elapsed, 200ms);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(TimeoutTest, ZeroTimeout) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 32> buf{};

        // Zero timeout - should fail immediately
        auto result = co_await AsyncRecv(ctx, sockets.fd2.Get(), buf, 0)
            .WithTimeout(0ms);

        EXPECT_FALSE(result.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// Timeout() Standalone Helper Tests
// -----------------------------------------------------------------------------

TEST_F(TimeoutTest, StandaloneTimeoutHelper) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 32> buf{};

        // Using standalone Timeout() instead of .WithTimeout()
        auto result = co_await Timeout(
            AsyncRecv(ctx, sockets.fd2.Get(), buf, 0),
            50ms
        );

        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), std::make_error_code(std::errc::timed_out));
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}