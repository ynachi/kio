//
// Created by Yao ACHI on 05/10/2025.
//
#include "kio/sync/sync_wait.h"

#include <gtest/gtest.h>
#include <stdexcept>

#include "kio/core/coro.h"

using namespace kio;

// Test: Basic void task
TEST(SyncWaitTest, VoidTask)
{
    bool executed = false;

    auto task = [&]() -> Task<void> {
        executed = true;
        co_return;
    };

    SyncWait(task());
    EXPECT_TRUE(executed);
}

// Test: Return int value
TEST(SyncWaitTest, IntReturnValue)
{
    auto task = []() -> Task<int> {
        co_return 42;
    };

    const int result = SyncWait(task());
    EXPECT_EQ(result, 42);
}

// Test: Return string value
TEST(SyncWaitTest, StringReturnValue)
{
    auto task = []() -> Task<std::string> {
        co_return "Hello, World!";
    };

    std::string result = SyncWait(task());
    EXPECT_EQ(result, "Hello, World!");
}

// Test: Task throws exception
TEST(SyncWaitTest, ExceptionPropagation)
{
    auto task = []() -> Task<int> {
        throw std::runtime_error("Test exception");
        co_return 42;
    };

    EXPECT_THROW({
        SyncWait(task());
    }, std::runtime_error);
}

// Test: Void task throws exception
TEST(SyncWaitTest, VoidTaskExceptionPropagation)
{
    auto task = []() -> Task<void> {
        throw std::logic_error("Void task error");
        co_return;
    };

    EXPECT_THROW({
        SyncWait(task());
    }, std::logic_error);
}

// Test: Nested tasks
TEST(SyncWaitTest, NestedTasks)
{
    auto inner_task = []() -> Task<int> {
        co_return 10;
    };

    auto outer_task = [&inner_task]() -> Task<int> {
        const int value = co_await inner_task();
        co_return value * 2;
    };

    const int result = SyncWait(outer_task());
    EXPECT_EQ(result, 20);
}

// Test: Multiple sync_waits in sequence
TEST(SyncWaitTest, MultipleSequentialCalls)
{
    auto task_factory = [](const int value) -> Task<int> {
        co_return value * 2;
    };

    const int result1 = SyncWait(task_factory(5));
    const int result2 = SyncWait(task_factory(10));
    const int result3 = SyncWait(task_factory(15));

    EXPECT_EQ(result1, 10);
    EXPECT_EQ(result2, 20);
    EXPECT_EQ(result3, 30);
}

// Test: Task with movable-only type
TEST(SyncWaitTest, MoveOnlyType)
{
    auto task = []() -> Task<std::unique_ptr<int>> {
        co_return std::make_unique<int>(42);
    };

    const auto result = SyncWait(task());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*result, 42);
}

// Test: Task that completes immediately
TEST(SyncWaitTest, ImmediateCompletion)
{
    auto task = []() -> Task<int> {
        co_return 123;
    };

    const auto start = std::chrono::steady_clock::now();
    const int result = SyncWait(task());
    const auto end = std::chrono::steady_clock::now();

    EXPECT_EQ(result, 123);
    // Should complete very quickly (less than 100ms)
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_LT(duration.count(), 100);
}

// Test: Complex return type (struct)
TEST(SyncWaitTest, StructReturnValue)
{
    struct TestData {
        int x;
        std::string y;

        bool operator==(const TestData& other) const {
            return x == other.x && y == other.y;
        }
    };

    auto task = []() -> Task<TestData> {
        co_return TestData{42, "test"};
    };

    auto [x, y] = SyncWait(task());
    EXPECT_EQ(x, 42);
    EXPECT_EQ(y, "test");
}

// Test: Task with reference return
TEST(SyncWaitTest, LargeObjectReturn)
{
    struct LargeObject {
        std::array<int, 1000> data{};

        LargeObject() {
            std::ranges::fill(data, 42);
        }
    };

    auto task = []() -> Task<LargeObject> {
        co_return LargeObject{};
    };

    const LargeObject result = SyncWait(task());
    EXPECT_EQ(result.data[0], 42);
    EXPECT_EQ(result.data[999], 42);
}

// Test: Multiple exceptions in sequence
TEST(SyncWaitTest, MultipleExceptions)
{
    auto task1 = []() -> Task<int> {
        throw std::runtime_error("Error 1");
        co_return 1;
    };

    auto task2 = []() -> Task<int> {
        throw std::logic_error("Error 2");
        co_return 2;
    };

    EXPECT_THROW({
        SyncWait(task1());
    }, std::runtime_error);

    EXPECT_THROW({
        SyncWait(task2());
    }, std::logic_error);
}

// Test: Task chain with exception in the middle
TEST(SyncWaitTest, NestedTaskWithException)
{
    auto failing_task = []() -> Task<int> {
        throw std::runtime_error("Inner error");
        co_return 10;
    };

    auto outer_task = [&failing_task]() -> Task<int> {
        const int value = co_await failing_task();
        co_return value * 2;  // Should never reach here
    };

    EXPECT_THROW({
        SyncWait(outer_task());
    }, std::runtime_error);
}

// Test: Empty task (immediate return)
TEST(SyncWaitTest, EmptyVoidTask)
{
    auto task = []() -> Task<void> {
        co_return;
    };

    EXPECT_NO_THROW({
        SyncWait(task());
    });
}

// Test: Task returning bool
TEST(SyncWaitTest, BoolReturnValue)
{
    auto task_true = []() -> Task<bool> {
        co_return true;
    };

    auto task_false = []() -> Task<bool> {
        co_return false;
    };

    EXPECT_TRUE(SyncWait(task_true()));
    EXPECT_FALSE(SyncWait(task_false()));
}

// Test: Task returning zero
TEST(SyncWaitTest, ZeroReturnValue)
{
    auto task = []() -> Task<int> {
        co_return 0;
    };

    const int result = SyncWait(task());
    EXPECT_EQ(result, 0);
}

// Test: Task returning negative value
TEST(SyncWaitTest, NegativeReturnValue)
{
    auto task = []() -> Task<int> {
        co_return -42;
    };

    const int result = SyncWait(task());
    EXPECT_EQ(result, -42);
}

// Test: Multiple awaits in single task
TEST(SyncWaitTest, MultipleAwaitsInTask)
{
    auto helper = [](int value) -> Task<int> {
        co_return value;
    };

    auto task = [&helper]() -> Task<int> {
        const int a = co_await helper(10);
        const int b = co_await helper(20);
        const int c = co_await helper(30);
        co_return a + b + c;
    };

    const int result = SyncWait(task());
    EXPECT_EQ(result, 60);
}

// Stress test: Many sequential sync_waits
TEST(SyncWaitTest, StressTestSequential)
{
    auto task_factory = [](const int i) -> Task<int> {
        co_return i * i;
    };

    for (int i = 0; i < 100; ++i) {
        int result = SyncWait(task_factory(i));
        EXPECT_EQ(result, i * i);
    }
}

// Test: Task with multiple co_returns (only first executes)
TEST(SyncWaitTest, ConditionalReturn)
{
    auto task = [](const bool condition) -> Task<int> {
        if (condition) {
            co_return 1;
        }
        co_return 2;
    };

    EXPECT_EQ(SyncWait(task(true)), 1);
    EXPECT_EQ(SyncWait(task(false)), 2);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
