#include "kio/task.h"

#include "kio/baton.h"

#include "kio/try.hpp"

#include <cstdio>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <absl/log/log_entry.h>
#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>
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
    explicit NoDefault(int vv) : v(vv) {}

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
}  // namespace

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
    (void)abandoned;  // scope ends, ~Task destroys the unstarted frame
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

// ===========================================================================
// Stack safety (symmetric transfer)
// ===========================================================================

Task<int> trivial_seven()
{
    co_return 7;
}

// One task co_awaiting an immediately-completing child 1e6 times: each
// iteration is a full suspend + symmetric-transfer pair. Without tail-call
// transfer in Final::await_suspend this unbounds the machine stack -- the
// point of running it at -O0 too (the dev preset).
Task<int> sequential_awaits(const int n)
{
    long sum = 0;
    for (int i = 0; i < n; ++i)
    {
        const auto r = co_await trivial_seven();
        sum += r.value_or(-1);
    }
    co_return static_cast<int>(sum);
}

TEST(TaskTest, MillionSequentialAwaitsDoNotOverflowTheStack)
{
    const auto res = run_to_result(sequential_awaits(1'000'000));
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 7'000'000);
}

// Deep co_await recursion: 100k frames are all live simultaneously (each
// parent waits for its child), so they must live on the heap and unwind via
// symmetric transfer, never nested resume() calls on the machine stack.
Task<int> recursive_depth(const int n)
{
    if (n <= 0)
    {
        co_return 0;
    }
    const auto r = co_await recursive_depth(n - 1);
    co_return r.value_or(-1) + 1;
}

TEST(TaskTest, DeepRecursionTransfersInsteadOfNesting)
{
    const auto res = run_to_result(recursive_depth(100'000));
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 100'000);
}

// ===========================================================================
// Manual-drive deaths and logging
// ===========================================================================

// A task parked mid-body on a never-posted Baton.
Task<> parked_on_baton(Baton& b)
{
    co_await b.wait();
    co_return {};
}

// Abandoning a started task whose frame is still suspended would leave its
// in-flight ops unwakeable: ~Task must abort instead.
TEST(TaskDeathTest, DestroyingSuspendedTaskTerminates)
{
    io::Context ctx{0, nullptr, io::IoOptions{}};
    Baton b{ctx};
    EXPECT_DEATH(
        {
            auto t = parked_on_baton(b);
            t.await_suspend(std::noop_coroutine()).resume();  // runs body -> parks
        },                                                    // scope end: ~Task sees started && !done
        "destroyed while suspended");
}

// A second awaiter would silently steal the first caller's continuation and
// strand it: one parent per task, CHECK-enforced.
Task<> await_ref(Task<>& inner, int& after)
{
    static_cast<void>(co_await inner);
    ++after;
    co_return {};
}

Task<> inner_parked(Baton& b)
{
    co_await b.wait();
    co_return {};
}

TEST(TaskDeathTest, SecondAwaiterOfASuspendedTaskTerminates)
{
    // batch_max_size 1 keeps the schedule deterministic: awaiter A runs to
    // its co_await (inner parks on the baton), then awaiter B hits the
    // occupied-continuation CHECK.
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 1;
    io::Context ctx{0, nullptr, opts};
    Baton b{ctx};

    EXPECT_DEATH(
        {
            auto inner = inner_parked(b);
            int a_after = 0;
            int b_after = 0;
            auto first = await_ref(inner, a_after);
            auto second = await_ref(inner, b_after);
            ctx.spawn(std::move(first));
            ctx.spawn(std::move(second));
            ctx.run();
        },
        "awaited by a second coroutine");
}

// An exception escaping a coroutine body reaches unhandled_exception, whose
// only legal move is std::terminate. A custom terminate handler marks the
// abort so the death test does not depend on libstdc++/libc++ wording.
Task<> throwing_body()
{
    throw std::runtime_error("kio test throw");
    co_return {};
}

TEST(TaskDeathTest, EscapingExceptionTerminates)
{
    EXPECT_DEATH(
        {
            std::set_terminate(
                []
                {
                    std::fputs("kio-test-terminate-marker\n", stderr);
                    std::abort();
                });
            auto t = throwing_body();
            t.await_suspend(std::noop_coroutine()).resume();
        },
        "kio-test-terminate-marker");
}

// gmock is not built (BUILD_GMOCK OFF), so ScopedMockLog is unavailable:
// a plain absl::LogSink captures what LOG(ERROR) emitted.
class CapturingLogSink final : public absl::LogSink
{
public:
    void Send(const absl::LogEntry& entry) override { messages_.emplace_back(entry.text_message()); }

    [[nodiscard]] bool saw_message_containing(const std::string_view needle) const
    {
        for (const auto& m : messages_)
        {
            if (m.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::string> messages_;
};

// A completed-but-unconsumed error result must not vanish with the frame:
// ~Task logs it. Driven manually (await_suspend + resume, no await_resume)
// because every real completion path consumes the result.
TEST(TaskLoggingTest, UnconsumedErrorResultIsLogged)
{
    CapturingLogSink sink;
    absl::AddLogSink(&sink);
    {
        auto t = error_task();
        t.await_suspend(std::noop_coroutine()).resume();  // runs to completion
    }  // ~Task: result_taken == false && error
    absl::RemoveLogSink(&sink);

    EXPECT_TRUE(sink.saw_message_containing("unconsumed error result"));
}

// spawn() detaches through run_detached: nobody will ever read the Result,
// so an error arriving there is logged, not dropped.
TEST(TaskLoggingTest, DetachedErrorResultIsLogged)
{
    CapturingLogSink sink;
    absl::AddLogSink(&sink);
    {
        io::Context ctx{0, nullptr, io::IoOptions{}};
        ctx.spawn(void_error_task());
        ctx.run();
    }
    absl::RemoveLogSink(&sink);

    EXPECT_TRUE(sink.saw_message_containing("detached Task discarded an error result"));
}

// ===========================================================================
// spawn
// ===========================================================================

namespace
{
int g_spawned = 0;

Task<> counting_spawn_noop()
{
    ++g_spawned;
    co_return {};
}

// Spawns 5000 children from INSIDE one task: the run loop resumes from
// its 256-entry stack chunk while the ready ring grows past its initial
// 1024 capacity -- the copy-out-before-resume contract in ready-ring.md.
Task<> spawn_storm(io::Context& io, const int n)
{
    for (int i = 0; i < n; ++i)
    {
        io.spawn(counting_spawn_noop());
    }
    co_return {};
}

Task<> spawn_from_inside(io::Context& io, int& outer_ran)
{
    io.spawn(counting_spawn_noop());
    outer_ran = g_spawned;  // children have NOT run yet: spawn only posts
    co_return {};
}
}  // namespace

TEST(TaskSpawnTest, SpawnFromInsideACoroutineRunsChildrenAfterwards)
{
    g_spawned = 0;
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx{0, nullptr, opts};

    int outer_ran = -1;
    ctx.spawn(spawn_from_inside(ctx, outer_ran));
    ctx.run();

    EXPECT_EQ(outer_ran, 0);  // spawn posts; it does not resume inline
    EXPECT_EQ(g_spawned, 1);
}

TEST(TaskSpawnTest, FiveThousandSpawnsGrowTheReadyRingMidTask)
{
    g_spawned = 0;
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx{0, nullptr, opts};

    ctx.spawn(spawn_storm(ctx, 5000));
    ctx.run();

    EXPECT_EQ(g_spawned, 5000);
}

// spawn() without a matching run() leaves work on the ready list: ~Context
// must abort (the handles would dangle after ring exit).
TEST(TaskSpawnDeathTest, SpawnWithoutRunAbortsAtContextTeardown)
{
    // opts lives outside: braced-init commas inside a macro argument would
    // be split by the preprocessor (it only respects parentheses).
    io::IoOptions opts;
    EXPECT_DEATH(
        {
            io::Context ctx(0, nullptr, opts);
            ctx.spawn(counting_spawn_noop());
        },
        "unstarted work");
}
