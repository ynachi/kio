// Error value + the KIO_TRY macro family.
//
// The macros compile to ordinary statements, so these tests pin both
// semantics and hygiene: error payloads propagate verbatim (code AND
// operation), two uses in one scope get distinct __COUNTER__ bindings, and
// the two footguns the external review called out are pinned as behavior:
// KIO_CO_TRY moves out of an lvalue argument, and an unbraced *_VOID macro
// swallows the caller's else.

#include "kio/error.hpp"

#include "kio/task.h"

#include "kio/try.hpp"

#include <cerrno>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

using namespace kio;

namespace
{
constexpr const char* kHelperOp = "helper_op";

Result<int> helper_ok(const int v)
{
    return v;
}

Result<int> helper_fail()
{
    return Error::fail_errc(std::errc::invalid_argument, kHelperOp);
}

Result<std::unique_ptr<int>> helper_move_only()
{
    return std::make_unique<int>(11);
}

Result<void> helper_void_ok()
{
    return {};
}

Result<void> helper_void_fail()
{
    return Error::fail_errno(EACCES, kHelperOp);
}

// ---- driving ----
// Same shape as task_test.cpp: a task runs when awaited from a root task
// that a Context drives; the root stores the awaited Result into `out`.

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

// ---- macro scenario bodies ----

Task<int> co_try_int()
{
    KIO_CO_TRY(v, helper_ok(41));
    co_return v + 1;
}

Task<std::unique_ptr<int>> co_try_move_only()
{
    KIO_CO_TRY(p, helper_move_only());
    co_return p;  // the unwrapped value moves on to the return
}

// The error converts across Task<U>/Result<T>: a Task<std::string> consumes
// a Result<int>'s failure, carrying code and operation unchanged.
Task<std::string> co_try_error_keeps_payload()
{
    KIO_CO_TRY(v, helper_fail());
    co_return std::to_string(v);  // unreachable: the macro already returned
}

Task<> co_try_void_ok()
{
    KIO_CO_TRY_VOID(helper_void_ok());
    co_return {};
}

Task<> co_try_void_error()
{
    KIO_CO_TRY_VOID(helper_void_fail());
    co_return {};  // unreachable
}

// Two uses in one scope: __COUNTER__ must give the two expansions distinct
// binding names, or the second would redeclare the first.
Task<int> co_try_twice_in_one_scope()
{
    KIO_CO_TRY(a, helper_ok(1));
    KIO_CO_TRY(b, helper_ok(2));
    co_return a + b;
}

Result<int> plain_try_ok()
{
    KIO_TRY(v, helper_ok(7));
    return v * 2;
}

Result<int> plain_try_fail()
{
    KIO_TRY(v, helper_fail());
    return v;  // unreachable
}

Result<void> plain_try_void_fail()
{
    KIO_TRY_VOID(helper_void_fail());
    return {};  // unreachable
}

// Footgun pin (review §3.2): KIO_CO_TRY binds auto&& to the ARGUMENT
// expression. Handed an lvalue, it moves the value out of the caller's
// object: `src` still has_value() (an expected holding a moved-from T), but
// its unique_ptr is null. Callers must pass rvalues.
Task<int> consume_lvalue(Result<std::unique_ptr<int>>& src)
{
    KIO_CO_TRY(p, src);
    co_return *p;
}

// Footgun pin (review §3.3): the *_VOID macros expand to if-statements.
// Without braces, the caller's `else` binds to the macro's INNER if, so the
// else-branch runs when the macro SUCCEEDED -- and does not run when the
// condition was false. This is the concrete case behind the always-brace
// rule. The pragma suppresses the compiler warning whose subject IS the
// pinned footgun.
#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdangling-else"
#endif
Result<void> dangling_else(const bool cond, bool& else_ran)
{
    if (cond)
        KIO_TRY_VOID(helper_void_ok());
    else
        else_ran = true;
    return {};
}
#if defined(__clang__)
    #pragma clang diagnostic pop
#endif
}  // namespace

// ---- Error ----

TEST(ErrorTest, FromErrnoMapsGenericCategoryAndValue)
{
    const auto e = Error::from_errno(EACCES, "open");
    EXPECT_EQ(e.value(), EACCES);
    EXPECT_EQ(e.code.category(), std::generic_category());
    EXPECT_EQ(e.context(), "open");
    EXPECT_TRUE(e);  // explicit operator bool: true MEANS error
}

TEST(ErrorTest, FromErrnoWithNullptrOperationBecomesEmpty)
{
    const auto e = Error::from_errno(EINVAL, nullptr);
    EXPECT_EQ(e.context(), "");
    EXPECT_EQ(e.value(), EINVAL);
}

TEST(ErrorTest, FromErrcMapsToErrcCategory)
{
    const auto e = Error::from_errc(std::errc::timed_out, "recv");
    EXPECT_EQ(e.value(), ETIMEDOUT);
    EXPECT_EQ(e.code.category(), std::generic_category());  // errc maps through generic
    EXPECT_TRUE(e.code == std::errc::timed_out);
}

TEST(ErrorTest, FailHelpersCarryTheSamePayload)
{
    const auto u1 = Error::fail_errno(EBADF, "close");
    static_assert(std::is_same_v<std::decay_t<decltype(u1)>, std::unexpected<Error>>);
    EXPECT_EQ(u1.error().value(), EBADF);
    EXPECT_EQ(u1.error().context(), "close");

    const auto u2 = Error::fail_errc(std::errc::no_such_file_or_directory, "open");
    EXPECT_EQ(u2.error().value(), ENOENT);
    EXPECT_EQ(u2.error().context(), "open");
}

TEST(ErrorTest, DefaultErrorIsNotAnError)
{
    const Error e{};
    EXPECT_FALSE(static_cast<bool>(e));
    EXPECT_EQ(e.value(), 0);
    EXPECT_EQ(e.context(), "");
}

TEST(ErrorTest, EqualityOverloads)
{
    const auto a = Error::from_errno(EACCES, "a");
    const auto same_code_other_context = Error::from_errno(EACCES, "different-context");
    const auto other_code = Error::from_errno(EBADF, "a");

    // Error == Error compares code only: operation is diagnostic context.
    EXPECT_TRUE(a == same_code_other_context);
    EXPECT_FALSE(a == other_code);

    EXPECT_TRUE(a == std::errc::permission_denied);
    EXPECT_FALSE(a == std::errc::invalid_argument);

    EXPECT_TRUE(a == a.code);
    EXPECT_TRUE(a.code == std::error_code(EACCES, std::generic_category()));
}

// ---- KIO_CO_TRY ----

TEST(CoTryTest, UnwrapsIntValue)
{
    const auto res = run_to_result(co_try_int());
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 42);
}

TEST(CoTryTest, UnwrapsMoveOnlyValue)
{
    auto res = run_to_result(co_try_move_only());
    ASSERT_TRUE(res.has_value());
    ASSERT_NE(*res, nullptr);
    EXPECT_EQ(**res, 11);
}

TEST(CoTryTest, PropagatesErrorWithIdenticalCodeAndOperation)
{
    auto res = run_to_result(co_try_error_keeps_payload());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EINVAL);
    EXPECT_EQ(res.error().context(), kHelperOp);
}

TEST(CoTryTest, VoidFormAcceptsSuccess)
{
    const auto res = run_to_result(co_try_void_ok());
    ASSERT_TRUE(res.has_value());
}

TEST(CoTryTest, VoidFormPropagatesError)
{
    auto res = run_to_result(co_try_void_error());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EACCES);
    EXPECT_EQ(res.error().context(), kHelperOp);
}

TEST(CoTryTest, TwoUsesInOneScopeGetDistinctBindings)
{
    const auto res = run_to_result(co_try_twice_in_one_scope());
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 3);
}

TEST(CoTryTest, LvalueArgumentIsMovedFrom)
{
    io::Context ctx{0, nullptr, io::IoOptions{}};
    Result<std::unique_ptr<int>> src{std::make_unique<int>(5)};
    Result<int> out{0};
    ctx.spawn(root(consume_lvalue(src), out));
    ctx.run();

    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 5);
    // The pinned footgun: the caller's object was silently emptied. If this
    // ever changes (e.g. the macro starts rejecting lvalues), the test must
    // change with it -- that is the point.
    EXPECT_TRUE(src.has_value());
    EXPECT_EQ(src->get(), nullptr);
}

// ---- KIO_TRY / KIO_TRY_VOID in plain functions ----

TEST(TryMacroTest, PlainTryUnwrapsAndPropagates)
{
    const auto ok = plain_try_ok();
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 14);

    const auto fail = plain_try_fail();
    ASSERT_FALSE(fail.has_value());
    EXPECT_EQ(fail.error().value(), EINVAL);
    EXPECT_EQ(fail.error().context(), kHelperOp);
}

TEST(TryMacroTest, PlainTryVoidPropagates)
{
    const auto res = plain_try_void_fail();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EACCES);
}

TEST(TryMacroTest, UnbracedElseBindsToTheMacroIf)
{
    bool else_ran = false;

    // The macro SUCCEEDS here, and yet the else-branch runs: it bound to the
    // macro's inner if, not to `cond`.
    EXPECT_TRUE(dangling_else(true, else_ran).has_value());
    EXPECT_TRUE(else_ran) << "else ran although the macro succeeded (bound to the inner if)";

    // And with the condition false, the else does NOT run.
    else_ran = false;
    EXPECT_TRUE(dangling_else(false, else_ran).has_value());
    EXPECT_FALSE(else_ran) << "else skipped although the condition was false";
}
