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
Task<> recv_hold_op(io::Context& io, io::UniqueFd fd, detail::IoAwaitableBase** slot, Result<int>& out,
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
Task<> recv_hold_op_then_park(io::Context& io, io::UniqueFd fd, detail::IoAwaitableBase** slot, Result<int>& out,
                              Baton& done)
{
    std::byte b{};
    auto op = async_recv(io, fd, std::span{&b, 1});
    *slot = &op;
    out = co_await op;
    co_await done.wait();
    co_return {};
}

Task<> cancel_op_after(io::Context& io, detail::IoAwaitableBase** slot, const std::chrono::milliseconds delay)
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
    detail::IoAwaitableBase* slot = nullptr;
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
    detail::IoAwaitableBase* slot = nullptr;
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
    detail::IoAwaitableBase* slot = nullptr;
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
    detail::IoAwaitableBase* slot = nullptr;
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

// ===========================================================================
// cancel_all semantics
// ===========================================================================

// Teardown with armed link timeouts: every canceled op must report
// ECANCELED ("operation canceled") because canceling_all_ is latched --
// never a fake ETIMEDOUT, even though each op carried a .timeout().
TEST(TimingCancelTest, CancelAllWithArmedTimeoutsReportsCanceledNotTimeout)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    io::UniqueFd peer{sv[1]};

    io::Context ctx(0, nullptr, test_opts());
    Result<int> out1{0};
    Result<int> out2{0};
    Result<int> out3{0};
    detail::IoAwaitableBase* s1 = nullptr;
    detail::IoAwaitableBase* s2 = nullptr;
    detail::IoAwaitableBase* s3 = nullptr;
    ctx.spawn(recv_hold_op(ctx, io::UniqueFd{sv[0]}, &s1, out1, 5s));
    ctx.spawn(recv_hold_op(ctx, io::UniqueFd{sv[0]}, &s2, out2, 5s));
    ctx.spawn(recv_hold_op(ctx, io::UniqueFd{sv[0]}, &s3, out3, 5s));
    ctx.spawn(cancel_all_after(ctx, 10ms));
    ctx.run();  // must return

    for (const auto* out : {&out1, &out2, &out3})
    {
        ASSERT_FALSE(out->has_value());
        EXPECT_EQ(out->error().value(), ECANCELED);
        EXPECT_EQ(out->error().context(), "operation canceled");
    }
}

// canceling_all_ resets at the TOP of run(): a second session must again
// classify a fired link timeout as ETIMEDOUT (not inherit the teardown
// latch from session one).
TEST(TimingCancelTest, SecondRunClassifiesTimeoutsAgain)
{
    int sv1[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv1), 0);
    io::UniqueFd p1{sv1[1]};

    io::Context ctx(0, nullptr, test_opts());

    // Session one: teardown cancel -> ECANCELED.
    Result<int> first{0};
    ctx.spawn(recv_hold_op_forever(ctx, io::UniqueFd{sv1[0]}, first));
    ctx.spawn(cancel_all_after(ctx, 10ms));
    ctx.run();
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error().context(), "operation canceled");

    // Session two, same Context: a fresh recv under a real timeout fires
    // ETIMEDOUT again.
    int sv2[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv2), 0);
    io::UniqueFd p2{sv2[1]};
    Result<int> second{0};
    detail::IoAwaitableBase* slot = nullptr;
    ctx.spawn(recv_hold_op(ctx, io::UniqueFd{sv2[0]}, &slot, second, 50ms));
    ctx.run();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().value(), ETIMEDOUT);
    EXPECT_EQ(second.error().context(), "operation timed out");
}

TEST(TimingCancelTest, CancelAllBeforeAnyRunIsHarmless)
{
    io::Context ctx(0, nullptr, test_opts());
    ctx.cancel_all();  // SQE queued, never yet submitted

    int v = 0;
    ctx.spawn(bump(v));
    ctx.run();  // must complete normally
    EXPECT_EQ(v, 1);
}

// ===========================================================================
// cancel races
// ===========================================================================

// Deterministic lost race: the data is queued BEFORE run(), so the recv
// completes as soon as it is submitted -- while the cancel task is still
// sleeping. The late cancel finds the op Done and must no-op: A sees
// success, not cancellation. The frame parks on a Baton afterwards so the
// slot stays valid until both cancels are long done.
TEST(TimingCancelTest, CancelThatLosesTheRaceLeavesTheOpSuccessful)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    const char c = 'x';
    ASSERT_EQ(::write(sv[1], &c, 1), 1);  // queued BEFORE run: race is decided early

    io::Context ctx(0, nullptr, test_opts());
    Baton done(ctx);
    Result<int> out{0};
    detail::IoAwaitableBase* slot = nullptr;
    ctx.spawn(recv_hold_op_then_park(ctx, io::UniqueFd{sv[0]}, &slot, out, done));
    ctx.spawn(cancel_op_after(ctx, &slot, 1ms));
    ctx.spawn(post_baton_after(ctx, done, 30ms));
    ctx.run();

    ASSERT_TRUE(out.has_value()) << "recv with pre-queued data must win the race";
    EXPECT_EQ(*out, 1);
    EXPECT_EQ(::close(sv[1]), 0);
}

// Two cancels on the same op: the second sees cancel_requested (or Done)
// and submits nothing. Harmless, and the op reports one clean ECANCELED.
// The frame parks on a Baton after the op completes so the slot stays
// valid for the second cancel.
TEST(TimingCancelTest, DoubleCancelIsHarmless)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    io::UniqueFd peer{sv[1]};

    io::Context ctx(0, nullptr, test_opts());
    Baton done(ctx);
    Result<int> out{0};
    detail::IoAwaitableBase* slot = nullptr;
    ctx.spawn(recv_hold_op_then_park(ctx, io::UniqueFd{sv[0]}, &slot, out, done));
    ctx.spawn(cancel_op_after(ctx, &slot, 10ms));
    ctx.spawn(cancel_op_after(ctx, &slot, 20ms));
    ctx.spawn(post_baton_after(ctx, done, 50ms));
    ctx.run();

    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().value(), ECANCELED);
    EXPECT_EQ(out.error().context(), "operation canceled");
}

// The stray link-timeout CQE from a completed op is tagged IGNORE_TAG and
// must be skipped by reap(), never misread as an op completion.
TEST(TimingCancelTest, StrayTimeoutCqeIsSwallowed)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    io::Context ctx(0, nullptr, test_opts());
    Result<int> out{0};
    detail::IoAwaitableBase* slot = nullptr;
    ctx.spawn(recv_hold_op(ctx, io::UniqueFd{sv[0]}, &slot, out, 20ms));
    ctx.spawn(write_byte_after(ctx, io::UniqueFd{sv[1]}, 5ms));
    ctx.run();

    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 1);

    // A second session reaps whatever stray timeout CQEs are still queued;
    // the IGNORE_TAG skip must make this a no-op, not a crash.
    int v = 0;
    ctx.spawn(bump(v));
    ctx.run();
    EXPECT_EQ(v, 1);
}

// A 1h sleep under a 10ms link timeout must return ETIMEDOUT promptly.
// This is the scratch_/link_ts_ non-union pin: if the two timespecs shared
// storage, the sleep's prep would overwrite the link timeout and the test
// would hang until the ctest TIMEOUT.
namespace
{
Task<> long_sleep_under_short_timeout(io::Context& io, Result<int>& out)
{
    out = co_await async_sleep(io, 1h).timeout(10ms);
    co_return {};
}
}  // namespace

TEST(TimingCancelTest, LongSleepUnderShortTimeoutReportsEtimedout)
{
    io::Context ctx(0, nullptr, test_opts());

    Result<int> out{0};
    const auto start = std::chrono::steady_clock::now();
    ctx.spawn(long_sleep_under_short_timeout(ctx, out));
    ctx.run();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().value(), ETIMEDOUT);
    EXPECT_EQ(out.error().context(), "operation timed out");
    EXPECT_LE(elapsed, std::chrono::seconds(10)) << "the 1h sleep must have been cut short";
}
