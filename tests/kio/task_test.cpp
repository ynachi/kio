#include "kio/task.h"
#include "kio/try.hpp"

#include <gtest/gtest.h>

using namespace kio;

namespace
{
    Task<int> value_task()
    {
        co_return 42;
    }

    Task<int> error_task()
    {
        co_return Error::fail_errno(EINVAL);
    }

    // An Error converts to Result<T> implicitly: no unexpected wrapper needed.
    Task<int> direct_error_task()
    {
        co_return Error::fail_errno(EINVAL);
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

    // co_return; does not compile on a Task<void> (the promise has no
    // return_void): every exit names its outcome. co_return {}; is the
    // success form.
    Task<> void_error_task()
    {
        co_return Error::fail_errno(EINVAL);
    }

    Task<> void_value_task()
    {
        co_return {};
    }

    // The promise seeds its Result with a sentinel error rather than
    // default-initializing, so T needs no default constructor.
    struct NoDefault
    {
        explicit NoDefault(int vv) : v(vv)
        {
        }

        int v;
    };

    Task<NoDefault> non_default_task()
    {
        co_return NoDefault{7};
    }

    // Awaits the same frame twice: the second consume must terminate rather
    // than hand back a moved-from Result.
    Task<> double_await_task()
    {
        Task<int> t = value_task();
        auto first = co_await t;
        auto second = co_await t;
        (void)first;
        (void)second;
        co_return {};
    }

    // ---- driving ----
    //
    // v2 has no Task::get(): a task runs when awaited from a root task that a
    // Context drives. The root stores the awaited Result into `out`, which
    // outlives the root's frame because run_to_result() returns only after
    // ctx.run() has finished.

    template <class T>
    Task<> root(Task<T> t, Result<T>& out)
    {
        out = co_await t;
        co_return {};
    }

    template <class T>
    Result<T> run_to_result(Task<T> t)
    {
        io::Context ctx{0, nullptr, io::IoOptions{}};
        Result<T> out{std::unexpect, Error::from_errc(std::errc::operation_canceled)};
        ctx.spawn(root(std::move(t), out));
        ctx.run();
        return out;
    }
} // namespace

TEST(TaskTest, ValueTaskProducesResult)
{
    const auto res = run_to_result(value_task());
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 42);
}

TEST(TaskTest, CoAwaitReturnsResult)
{
    const auto res = run_to_result(await_value_task());
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 43);
}

TEST(TaskTest, CoAwaitPropagatesUnexpected)
{
    auto res = run_to_result(await_error_task());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EINVAL);
}

TEST(TaskTest, ErrorDirectCoReturn)
{
    auto res = run_to_result(direct_error_task());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EINVAL);
}

TEST(TaskTest, VoidTaskCanReturnResultVoid)
{
    const auto res = run_to_result(void_value_task());
    ASSERT_TRUE(res.has_value());
}

TEST(TaskTest, VoidTaskCanReturnUnexpected)
{
    auto res = run_to_result(void_error_task());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EINVAL);
}

TEST(TaskTest, NonDefaultConstructibleValueType)
{
    const auto res = run_to_result(non_default_task());
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->v, 7);
}

// Awaiting a moved-from Task reaches await_resume through await_ready(); the
// CHECK must turn that null-handle dereference into a deterministic abort.
TEST(TaskTest, AwaitEmptyTaskTerminates)
{
    EXPECT_DEATH(
        {
            Task<int> empty;
            (void)run_to_result(std::move(empty));
        },
        "empty or unfinished");
}

// The Result is single-consume.
TEST(TaskTest, DoubleConsumeTerminates)
{
    EXPECT_DEATH(run_to_result(double_await_task()), "consumed twice");
}

// Dropping a Task that was never co_awaited is legal: the frame is suspended
// at initial_suspend, nothing is in flight. Before the started flag this
// aborted in ~Task with "destroyed while suspended".
TEST(TaskTest, DropUnstartedTaskIsLegal)
{
    Task<int> abandoned = value_task();
    (void)abandoned; // scope ends, ~Task destroys the unstarted frame
}

// ---- KIO_CO_TRY / KIO_CO_TRY_VOID / KIO_TRY / KIO_TRY_VOID ----
//
// The _VOID forms exist because KIO_CO_TRY ends in `auto var = <unwrap>`,
// which is ill-formed for a Result<void> expression -- exactly the shape of
// every void op (fsync/close/rename/...).

Task<int> try_value()
{
    KIO_CO_TRY(v, co_await value_task());
    co_return v + 1;
}

Task<> try_void_ok()
{
    KIO_CO_TRY_VOID(co_await void_value_task());
    co_return {};
}

Task<> try_void_error()
{
    KIO_CO_TRY_VOID(co_await void_error_task());
    co_return {};  // unreachable: the macro returned the error
}

Result<int> plain_try()
{
    KIO_TRY(v, Result<int>{42});
    return v + 1;
}

Result<void> plain_try_void_error()
{
    KIO_TRY_VOID(Result<void>{std::unexpect, Error::from_errc(std::errc::invalid_argument)});
    return {};
}

TEST(TaskTest, CoTryUnwrapsValue)
{
    const auto res = run_to_result(try_value());
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 43);
}

TEST(TaskTest, CoTryVoidAcceptsSuccess)
{
    const auto res = run_to_result(try_void_ok());
    ASSERT_TRUE(res.has_value());
}

TEST(TaskTest, CoTryVoidPropagatesError)
{
    auto res = run_to_result(try_void_error());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EINVAL);
}

TEST(TaskTest, TryVoidPropagatesErrorOutsideCoroutines)
{
    auto res = plain_try_void_error();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EINVAL);
    EXPECT_EQ(plain_try().value(), 43);
}
