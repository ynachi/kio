#include <gtest/gtest.h>
#include "aio/aio.hpp"

using namespace aio;

TEST(IoContextTest, Lifecycle) {
    IoContext ctx;
    Task<> t = []() -> Task<> { co_return; }();
    ctx.RunUntilDone(t);
}

TEST(IoContextTest, ReturnValue) {
    IoContext ctx;
    Task<int> t = []() -> Task<int> { co_return 42; }();
    ctx.RunUntilDone(t);
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
    
    ctx.RunUntilDone(task);
    t.join();
    
    ASSERT_TRUE(notified);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}