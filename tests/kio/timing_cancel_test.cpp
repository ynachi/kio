// Timeout, cancel, and teardown classification -- the branch cluster in
// IoAwaitable::await_resume that decides what -ECANCELED means:
//   fired linked timeout          -> ETIMEDOUT ("operation timed out")
//   explicit cancel(op)           -> ECANCELED ("operation canceled")
//   teardown-wide cancel_all()    -> ECANCELED, and run() RETURNS
// plus the Context::spawn cross-thread CHECK (debug builds) and the
// leader/ATTACH_WQ construction path.
//
// Rings are tiny (64 entries) like the other uring tests. EXPECT (not
// ASSERT) inside coroutine bodies: ASSERT_* expands to a plain `return`,
// which is ill-formed there.

#include "kio/baton.h"
#include "kio/context.h"
#include "kio/operations.h"
#include "kio/task.h"

#include <chrono>
#include <span>
#include <thread>

#include <sys/socket.h>

#include <gtest/gtest.h>

using namespace kio;
using namespace std::chrono_literals;

namespace
{
// Tiny ring + small epoch cap, like every uring test here.
io::IoOptions test_opts()
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    return opts;
}

Task<> bump(int& v)
{
    ++v;
    co_return {};
}

Task<> write_byte_after(io::Context& io, io::UniqueFd fd, const std::chrono::milliseconds delay)
{
    co_await async_sleep(io, delay);
    const char c = 'x';
    EXPECT_EQ(::write(fd.get(), &c, 1), 1);
    co_return {};
}

// Holds the awaiter as an LVALUE (guaranteed copy elision -- no move of the
// non-movable IoAwaitable ever happens), publishes its address, then awaits
// it. The timer task cancels through that address: the pattern the free
// io::cancel(io, op) requires.
Task<> recv_hold_op(io::Context& io, io::UniqueFd fd, io::IoAwaitableBase** slot, Result<int>& out,
                    const std::chrono::nanoseconds timeout)
{
    std::byte b{};
    // auto op = <prvalue> is copy-init with guaranteed elision (no move of
    // the non-movable awaiter); .timeout() then mutates the named object.
    // NEVER `auto&& op = f(...).timeout(t)` -- binding through timeout()'s
    // returned reference does NOT lifetime-extend: the awaiter temporary
    // dies at the semicolon and the SQE tags dangling memory.
    auto op = async_recv(io, fd, std::span{&b, 1});
    op.timeout(timeout);
    *slot = &op;
    out = co_await op;
    co_return {};
}

// Same, without a timeout: for the teardown-wide cancel_all() path.
Task<> recv_hold_op_forever(io::Context& io, io::UniqueFd fd, Result<int>& out)
{
    std::byte b{};
    auto op = async_recv(io, fd, std::span{&b, 1});
    out = co_await op;
    co_return {};
}

// Park after the recv completes, with the awaiter still frame-resident: a
// late cancel(io, op) must be a guarded no-op, not a write into an op whose
// result was already consumed.
Task<> recv_hold_op_then_park(io::Context& io, io::UniqueFd fd, io::IoAwaitableBase** slot, Result<int>& out,
                              Baton& done)
{
    std::byte b{};
    auto op = async_recv(io, fd, std::span{&b, 1});
    *slot = &op;
    out = co_await op;
    co_await done.wait();
    co_return {};
}

Task<> cancel_op_after(io::Context& io, io::IoAwaitableBase** slot, const std::chrono::milliseconds delay)
{
    co_await async_sleep(io, delay);
    io::cancel(io, **slot);
    co_return {};
}

Task<> cancel_all_after(io::Context& io, const std::chrono::milliseconds delay)
{
    co_await async_sleep(io, delay);
    io.cancel_all();
    co_return {};
}

Task<> post_baton_after(io::Context& io, Baton& b, const std::chrono::milliseconds delay)
{
    co_await async_sleep(io, delay);
    b.post();
    co_return {};
}

Task<> sleep_under_timeout(io::Context& io, Result<int>& o)
{
    o = co_await async_sleep(io, 10ms).timeout(1s);
    co_return {};
}

// v1 behavior pin: a negative duration means "already elapsed" -- it must
// complete immediately with success, not surface the kernel's EINVAL for a
// timespec with negative tv_nsec.
Task<> negative_sleep(io::Context& io, Result<int>& o)
{
    o = co_await async_sleep(io, std::chrono::nanoseconds{-5'000'000});
    co_return {};
}
}  // namespace

// ---- linked timeout ----

// No data ever arrives; the armed link timeout fires at 50ms and the
// primary recv surfaces -ECANCELED -- which, unflagged, IS a timeout.
TEST(TimingCancelTest, LinkedTimeoutReportsEtimedout)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    io::Context ctx(0, nullptr, test_opts());
    Result<int> out{0};
    io::IoAwaitableBase* slot = nullptr;
    ctx.spawn(recv_hold_op(ctx, io::UniqueFd{sv[0]}, &slot, out, 50ms));
    ctx.run();

    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().value(), ETIMEDOUT);
    EXPECT_EQ(out.error().context(), "operation timed out");
}

// The op completes before the (generous) timeout: plain success, and the
// linked-timeout CQE is consumed as IGNORE_TAG without misclassification.
TEST(TimingCancelTest, CompletesUnderGenerousTimeout)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    io::Context ctx(0, nullptr, test_opts());
    Result<int> out{0};
    io::IoAwaitableBase* slot = nullptr;
    ctx.spawn(recv_hold_op(ctx, io::UniqueFd{sv[0]}, &slot, out, 1s));
    ctx.spawn(write_byte_after(ctx, io::UniqueFd{sv[1]}, 10ms));
    ctx.run();

    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 1);
}

// A sleep under a generous timeout: -ETIME-as-success must not masquerade
// as a timeout failure.
TEST(TimingCancelTest, SleepCompletesUnderGenerousTimeout)
{
    io::Context ctx(0, nullptr, test_opts());

    Result<int> out{0};
    ctx.spawn(sleep_under_timeout(ctx, out));
    ctx.run();

    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 0);
}

// Negative durations clamp to zero ("already elapsed"), restoring the v1
// contract the timespec split used to break with EINVAL.
TEST(TimingCancelTest, NegativeSleepCompletesImmediately)
{
    io::Context ctx(0, nullptr, test_opts());

    Result<int> out{0};
    ctx.spawn(negative_sleep(ctx, out));
    ctx.run();

    ASSERT_TRUE(out.has_value()) << "negative duration must not EINVAL";
    EXPECT_EQ(*out, 0);
}

// ---- explicit cancel ----

// cancel(op) on an op armed WITH a link timeout: cancel_requested() must
// win over the timeout classification -- ECANCELED, not a fake ETIMEDOUT.
TEST(TimingCancelTest, ExplicitCancelReportsCanceledNotTimeout)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    io::Context ctx(0, nullptr, test_opts());
    Result<int> out{0};
    io::IoAwaitableBase* slot = nullptr;
    ctx.spawn(recv_hold_op(ctx, io::UniqueFd{sv[0]}, &slot, out, 5s));
    ctx.spawn(cancel_op_after(ctx, &slot, 10ms));
    ctx.run();

    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().value(), ECANCELED);
    EXPECT_EQ(out.error().context(), "operation canceled");
}

// cancel(op) after the op is already Done must be a guarded no-op: no SQE,
// no flag write, and the (frame-resident) awaiter is only read.
TEST(TimingCancelTest, CancelAfterCompletionIsNoop)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    io::Context ctx(0, nullptr, test_opts());
    Baton done(ctx);
    Result<int> out{0};
    io::IoAwaitableBase* slot = nullptr;
    ctx.spawn(recv_hold_op_then_park(ctx, io::UniqueFd{sv[0]}, &slot, out, done));
    ctx.spawn(write_byte_after(ctx, io::UniqueFd{sv[1]}, 10ms));
    ctx.spawn(cancel_op_after(ctx, &slot, 50ms));
    ctx.spawn(post_baton_after(ctx, done, 80ms));
    ctx.run();

    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 1);
}

// ---- teardown ----

// cancel_all() must unblock a permanently-blocked recv and make run()
// RETURN -- this is the v2 teardown story: bounded by the timer, not a
// grace period, and the canceled op reports ECANCELED (not a fake timeout).
TEST(TimingCancelTest, CancelAllUnblocksRun)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    io::Context ctx(0, nullptr, test_opts());
    Result<int> out{0};
    ctx.spawn(recv_hold_op_forever(ctx, io::UniqueFd{sv[0]}, out));
    ctx.spawn(cancel_all_after(ctx, 10ms));
    ctx.run();  // must return

    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().value(), ECANCELED);
    EXPECT_EQ(out.error().context(), "operation canceled");
}

// ---- construction paths ----

// The follower attaches to the leader's work queue (ATTACH_WQ); both rings
// are created R_DISABLED, and each run() binds its own single issuer.
TEST(TimingCancelTest, SharedWorkQueueFollowerAttachesToLeader)
{
    io::Context leader(0, nullptr, test_opts());
    io::Context follower(1, &leader, test_opts());

    int a = 0;
    int b = 0;
    leader.spawn(bump(a));
    follower.spawn(bump(b));
    leader.run();
    follower.run();

    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);
}

// A malformed pool config must abort at the Context constructor -- naming
// the config -- before the pool maps anything.
TEST(TimingCancelTest, BadPoolSlotSizeAbortsAtConstruction)
{
    EXPECT_DEATH(
        { io::Context ctx(0, nullptr, test_opts(), BufferPoolConfig{.slot_size = 3000, .slots = 4}); }, "power of two");
}

// ---- loop-thread contract (debug builds only: check_loop_thread is
// compiled out under NDEBUG) ----

#ifndef NDEBUG
TEST(TimingCancelTest, CrossThreadSpawnAbortsInDebug)
{
    io::Context ctx(0, nullptr, test_opts());
    ctx.run();  // noop session: binds the loop-thread identity to this thread

    EXPECT_DEATH(
        {
            std::thread t(
                [&ctx]
                {
                    int v = 0;
                    ctx.spawn(bump(v));
                });
            t.join();
        },
        "loop-thread contract");
}
#endif
