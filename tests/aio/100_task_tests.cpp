// test tasks without any io involved

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "aio/task.hpp"

using namespace aio;

// -----------------------------------------------------------------------------
// Task<T> (non-void) Tests
// -----------------------------------------------------------------------------

TEST(TaskTest, ReturnsIntValue) {
    auto coro = []() -> Task<int> {
        co_return 42;
    };

    auto task = coro();
    EXPECT_FALSE(task.Done());

    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_EQ(task.Result(), 42);
}

TEST(TaskTest, ReturnsStringValue) {
    auto coro = []() -> Task<std::string> {
        co_return "hello world";
    };

    auto task = coro();
    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_EQ(task.Result(), "hello world");
}

TEST(TaskTest, PropagatesException) {
    auto coro = []() -> Task<int> {
        throw std::runtime_error("test error");
        co_return 0;  // Never reached
    };

    auto task = coro();
    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_THROW(task.Result(), std::runtime_error);
}

TEST(TaskTest, PropagatesSpecificException) {
    auto coro = []() -> Task<int> {
        throw std::logic_error("logic error");
        co_return 0;
    };

    auto task = coro();
    task.resume();

    EXPECT_TRUE(task.Done());
    try {
        task.Result();
        FAIL() << "Expected std::logic_error";
    } catch (const std::logic_error& e) {
        EXPECT_STREQ(e.what(), "logic error");
    }
}

TEST(TaskTest, MoveConstruction) {
    auto coro = []() -> Task<int> { co_return 123; };

    auto t1 = coro();
    auto t2 = std::move(t1);

    t2.resume();
    EXPECT_TRUE(t2.Done());
    EXPECT_EQ(t2.Result(), 123);
}

TEST(TaskTest, MoveAssignment) {
    auto coro = []() -> Task<int> { co_return 456; };

    Task<int> t1 = coro();
    Task<int> t2 = coro();

    t2 = std::move(t1);
    t2.resume();

    EXPECT_EQ(t2.Result(), 456);
}

TEST(TaskTest, StartAlias) {
    auto coro = []() -> Task<int> { co_return 99; };

    auto task = coro();
    task.Start();  // Alias for resume()

    EXPECT_TRUE(task.Done());
    EXPECT_EQ(task.Result(), 99);
}

TEST(TaskTest, NestedAwait) {
    auto inner = []() -> Task<int> {
        co_return 10;
    };

    auto outer = [&]() -> Task<int> {
        int x = co_await inner();
        co_return x * 2;
    };

    auto task = outer();
    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_EQ(task.Result(), 20);
}

TEST(TaskTest, MultipleNestedAwaits) {
    auto add = [](int a, int b) -> Task<int> {
        co_return a + b;
    };

    auto compute = [&]() -> Task<int> {
        int x = co_await add(1, 2);
        int y = co_await add(3, 4);
        int z = co_await add(x, y);
        co_return z;
    };

    auto task = compute();
    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_EQ(task.Result(), 10);  // (1+2) + (3+4) = 10
}

// -----------------------------------------------------------------------------
// Task<void> Tests
// -----------------------------------------------------------------------------

TEST(TaskVoidTest, CompletesSuccessfully) {
    bool executed = false;

    auto coro = [&]() -> Task<void> {
        executed = true;
        co_return;
    };

    auto task = coro();
    EXPECT_FALSE(executed);

    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_TRUE(executed);
}

TEST(TaskVoidTest, PropagatesException) {
    auto coro = []() -> Task<void> {
        throw std::runtime_error("void task error");
        co_return;
    };

    auto task = coro();
    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_THROW(task.Result(), std::runtime_error);
}

TEST(TaskVoidTest, MoveConstruction) {
    bool executed = false;

    auto coro = [&]() -> Task<void> {
        executed = true;
        co_return;
    };

    auto t1 = coro();
    auto t2 = std::move(t1);

    t2.resume();
    EXPECT_TRUE(t2.Done());
    EXPECT_TRUE(executed);
}

TEST(TaskVoidTest, StartAlias) {
    bool executed = false;

    auto coro = [&]() -> Task<void> {
        executed = true;
        co_return;
    };

    auto task = coro();
    task.Start();  // Alias for resume()

    EXPECT_TRUE(task.Done());
    EXPECT_TRUE(executed);
}

TEST(TaskVoidTest, NestedAwait) {
    int counter = 0;

    auto inner = [&]() -> Task<void> {
        counter += 1;
        co_return;
    };

    auto outer = [&]() -> Task<void> {
        co_await inner();
        co_await inner();
        co_await inner();
        co_return;
    };

    auto task = outer();
    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_EQ(counter, 3);
}

TEST(TaskVoidTest, AwaitValueTask) {
    auto value_task = []() -> Task<int> {
        co_return 42;
    };

    int captured = 0;
    auto void_task = [&]() -> Task<void> {
        captured = co_await value_task();
        co_return;
    };

    auto task = void_task();
    task.resume();

    EXPECT_TRUE(task.Done());
    EXPECT_EQ(captured, 42);
}

// -----------------------------------------------------------------------------
// Edge Cases
// -----------------------------------------------------------------------------

TEST(TaskTest, ImmediateReturn) {
    // Task that returns immediately with no suspension points
    auto coro = []() -> Task<int> {
        co_return 1;
    };

    auto task = coro();
    task.resume();

    EXPECT_TRUE(task.Done());
}

TEST(TaskTest, LargeReturnValue) {
    struct LargeStruct {
        std::array<int, 1000> data{};
    };

    auto coro = []() -> Task<LargeStruct> {
        LargeStruct s;
        s.data.fill(42);
        co_return s;
    };

    auto task = coro();
    task.resume();

    auto result = task.Result();
    EXPECT_EQ(result.data[0], 42);
    EXPECT_EQ(result.data[999], 42);
}

TEST(TaskTest, MoveOnlyReturnValue) {
    auto coro = []() -> Task<std::unique_ptr<int>> {
        co_return std::make_unique<int>(123);
    };

    auto task = coro();
    task.resume();

    auto result = task.Result();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*result, 123);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}