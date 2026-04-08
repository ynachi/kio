#include "kio/kio.hpp"

#include <gtest/gtest.h>

#include "test_helpers.hpp"

using namespace kio;
using namespace kio::test;

TEST(IoContextTest, Lifecycle) {
    IoContext ctx;
    Task<> t = []() -> Task<> { co_return; }();
    ctx.RunUntilDone(std::move(t));
}

TEST(IoContextTest, ReturnValue) {
    IoContext ctx;
    Task<int> t = []() -> Task<int> { co_return 42; }();
    ctx.RunUntilDone(std::move(t));
    ASSERT_EQ(t.Result(), 42);
}

TEST(IoContextTest, Notify) {
    IoContext ctx;
    bool notified = false;
    
    // Start a thread that waits a bit then notifies
    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        notified = ctx.Notify();
    });

    // Run a task that just sleeps (or waits for something)
    // Here we just use RunUntilDone with a simple task to ensure loop runs
    Task<> task = [&](IoContext& c) -> Task<> {
        co_await AsyncSleep(c, std::chrono::milliseconds(50));
    }(ctx);
    
    ctx.RunUntilDone(std::move(task));
    t.join();
    
    ASSERT_TRUE(notified);
}

TEST(MemoryIoContextTest, Lifecycle) {
    auto ctx = MakeMemoryIoContext();
    Task<int> t = []() -> Task<int> { co_return 7; }();
    ctx.RunUntilDone(std::move(t));
    ASSERT_EQ(t.Result(), 7);
}

TEST(MemoryIoContextTest, SleepZeroDuration) {
    auto ctx = MakeMemoryIoContext();
    auto task = [](MemoryIoContext& c) -> Task<void> {
        auto start = std::chrono::steady_clock::now();
        auto res = co_await AsyncSleep(c, std::chrono::milliseconds(0));
        EXPECT_TRUE(res.has_value());
        EXPECT_LE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(50));
        co_return;
    }(ctx);

    ctx.RunUntilDone(std::move(task));
}

TEST(MemoryIoContextTest, Notify) {
    auto ctx = MakeMemoryIoContext();
    bool notified = false;

    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        notified = ctx.Notify();
    });

    auto task = [](MemoryIoContext& c) -> Task<void> {
        auto res = co_await AsyncSleep(c, std::chrono::milliseconds(50));
        EXPECT_TRUE(res.has_value());
        co_return;
    }(ctx);

    ctx.RunUntilDone(std::move(task));
    t.join();

    ASSERT_TRUE(notified);
}

TEST(MemoryIoContextTest, AutoAdvanceSleepUsesSyntheticClock) {
    auto ctx = MakeMemoryIoContext();

    const auto start = ctx.GetBackend().Now();

    auto task = [](MemoryIoContext& c) -> Task<void> {
        auto res = co_await AsyncSleep(c, std::chrono::milliseconds(50));
        EXPECT_TRUE(res.has_value());
        co_return;
    }(ctx);

    ctx.RunUntilDone(std::move(task));

    EXPECT_EQ(ctx.GetBackend().Now() - start, std::chrono::milliseconds(50));
}

TEST(MemoryIoContextTest, ManualAdvanceSleepWaitsForExplicitAdvance) {
    auto ctx = MakeMemoryIoContext(
        MemoryBackendBuilder{}.WithTimeMode(MemoryBackend::TimeMode::ManualAdvance));

    const auto start = ctx.GetBackend().Now();

    auto task = [](MemoryIoContext& c) -> Task<void> {
        auto res = co_await AsyncSleep(c, std::chrono::milliseconds(50));
        EXPECT_TRUE(res.has_value());
        co_return;
    }(ctx);

    std::thread advancer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ctx.GetBackend().AdvanceTime(std::chrono::milliseconds(50));
        ctx.Notify();
    });

    ctx.RunUntilDone(std::move(task));
    advancer.join();

    EXPECT_EQ(ctx.GetBackend().Now() - start, std::chrono::milliseconds(50));
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
