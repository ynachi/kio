//
// Created by Yao ACHI on 05/10/2025.
//
#include "core/include/sync_wait.h"

#include <gtest/gtest.h>
#include <stdexcept>

#include "core/include/coro.h"

using namespace kio;
// Test coroutines
Task<int> get_answer() { co_return 42; }
Task<void> do_nothing()
{
    //sleep
     std::this_thread::sleep_for(std::chrono::milliseconds(20));
    co_return;
}
Task<int> throw_error() {
    throw std::runtime_error("Test error");
    co_return 0;
}

TEST(SyncWaitTest, ReturnsValue) {
    EXPECT_EQ(sync_wait(get_answer()), 42);
}

TEST(SyncWaitTest, HandlesVoidCoroutine) {
    EXPECT_NO_THROW(sync_wait(do_nothing()));
}

TEST(SyncWaitTest, PropagatesExceptions) {
    EXPECT_THROW(sync_wait(throw_error()), std::runtime_error);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

