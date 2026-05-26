#include "../../include/uring/core/task.hpp"

#include <cerrno>

#include <gtest/gtest.h>

using namespace URing;

namespace
{
Task<int> value_task()
{
    co_return 42;
}

Task<int> error_task()
{
    co_return std::unexpected(make_error_code(EINVAL));
}

Task<int> await_value_task()
{
    auto res = co_await value_task();
    if (!res)
    {
        co_return std::unexpected(res.error());
    }

    co_return *res + 1;
}

Task<int> await_error_task()
{
    auto res = co_await error_task();
    if (!res)
    {
        co_return std::unexpected(res.error());
    }

    co_return *res;
}

Task<void> void_error_task()
{
    co_return std::unexpected(make_error_code(EINVAL));
}

Task<void> void_value_task()
{
    co_return {};
}

void run_to_completion(auto& task)
{
    while (!task.done())
    {
        task.handle_.resume();
    }
}
}  // namespace

TEST(TaskTest, GetReturnsResult)
{
    auto task = value_task();
    run_to_completion(task);

    auto res = task.get();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 42);
}

TEST(TaskTest, CoAwaitReturnsResult)
{
    auto task = await_value_task();
    run_to_completion(task);

    auto res = task.get();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 43);
}

TEST(TaskTest, CoAwaitPropagatesUnexpected)
{
    auto task = await_error_task();
    run_to_completion(task);

    auto res = task.get();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EINVAL);
}

TEST(TaskTest, VoidTaskCanReturnUnexpected)
{
    auto task = void_error_task();
    run_to_completion(task);

    auto res = task.get();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EINVAL);
}

TEST(TaskTest, VoidTaskCanReturnResultVoid)
{
    auto task = void_value_task();
    run_to_completion(task);

    auto res = task.get();
    ASSERT_TRUE(res.has_value());
}
