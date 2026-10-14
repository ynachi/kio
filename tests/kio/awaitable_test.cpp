// IoAwaitableBase: the packed state byte (lifecycle low bits + flag bits)
// and await_resume()'s Result mapping.
//
// Everything here is black-box: `res`, `state`, and the flag setters are
// public, and await_resume() is a pure function of
// (res, flags, io.canceling_all_) -- no kernel involvement, because these
// ops are never suspended on the ring. The only awaited ops are inside the
// death tests, which drive the manual await_suspend(noop) form.

#include "kio/context.h"
#include "kio/operations.h"

#include <cstdint>

#include <sys/socket.h>

#include "test_util.hpp"
#include <gtest/gtest.h>

using namespace kio;

// The four flag bits must be disjoint from each other and from the
// lifecycle field they share the byte with.
static_assert((io::kLifecycleMask &
               (io::kEtimeIsSuccessBit | io::kHasLinkTimeoutBit | io::kCancelRequestedBit | io::kPreFailedBit)) == 0);
static_assert((io::kEtimeIsSuccessBit & io::kHasLinkTimeoutBit) == 0);
static_assert((io::kEtimeIsSuccessBit & io::kCancelRequestedBit) == 0);
static_assert((io::kEtimeIsSuccessBit & io::kPreFailedBit) == 0);
static_assert((io::kHasLinkTimeoutBit & io::kCancelRequestedBit) == 0);
static_assert((io::kHasLinkTimeoutBit & io::kPreFailedBit) == 0);
static_assert((io::kCancelRequestedBit & io::kPreFailedBit) == 0);

namespace
{
// An invocable that never runs: the mapping ops below are never suspended,
// so the prep never touches an SQE.
using NoPrep = void (*)(io_uring_sqe*, __kernel_timespec*);

Result<int> resume_with(io::Context& io, const int res, const bool etime, const bool link, const bool cancel_req,
                        const int pre_errno = 0)
{
    io::IoAwaitable<NoPrep> op{io, nullptr, etime, pre_errno};
    op.res = res;
    if (link)
    {
        op.set_has_link_timeout();
    }
    if (cancel_req)
    {
        op.set_cancel_requested();
    }
    return op.await_resume();
}
}  // namespace

TEST(IoAwaitableTest, DefaultState)
{
    io::IoAwaitableBase base;
    EXPECT_EQ(base.lifecycle(), io::IoAwaitableState::Created);
    EXPECT_FALSE(base.etime_is_success());
    EXPECT_FALSE(base.has_link_timeout());
    EXPECT_FALSE(base.cancel_requested());
    EXPECT_FALSE(base.pre_failed());
    EXPECT_EQ(base.res, 0);
}

TEST(IoAwaitableTest, LifecycleTransitionsPreserveFlags)
{
    io::IoAwaitableBase base;
    base.state |= io::kEtimeIsSuccessBit | io::kPreFailedBit;
    base.set_has_link_timeout();
    base.set_cancel_requested();

    for (const auto life : {io::IoAwaitableState::Created, io::IoAwaitableState::Inflight, io::IoAwaitableState::Done})
    {
        base.set_lifecycle(life);
        EXPECT_EQ(base.lifecycle(), life);
        EXPECT_TRUE(base.etime_is_success());
        EXPECT_TRUE(base.has_link_timeout());
        EXPECT_TRUE(base.cancel_requested());
        EXPECT_TRUE(base.pre_failed());
    }
}

TEST(IoAwaitableTest, FlagSettersPreserveLifecycleAndOtherFlags)
{
    io::IoAwaitableBase base;
    base.set_lifecycle(io::IoAwaitableState::Inflight);

    base.set_has_link_timeout();
    EXPECT_TRUE(base.has_link_timeout());
    EXPECT_EQ(base.lifecycle(), io::IoAwaitableState::Inflight);
    EXPECT_FALSE(base.cancel_requested());
    EXPECT_FALSE(base.etime_is_success());
    EXPECT_FALSE(base.pre_failed());

    base.set_cancel_requested();
    EXPECT_TRUE(base.cancel_requested());
    EXPECT_TRUE(base.has_link_timeout());
    EXPECT_EQ(base.lifecycle(), io::IoAwaitableState::Inflight);
    EXPECT_FALSE(base.etime_is_success());
    EXPECT_FALSE(base.pre_failed());
}

TEST(IoAwaitableTest, ConstructorSeedsFlagsAndPrefailErrno)
{
    io::Context ctx(0, nullptr, test::test_opts());

    io::IoAwaitable<NoPrep> normal{ctx, nullptr, /*etime_is_success=*/false, 0};
    EXPECT_FALSE(normal.etime_is_success());
    EXPECT_FALSE(normal.pre_failed());
    EXPECT_EQ(normal.res, 0);
    EXPECT_EQ(normal.lifecycle(), io::IoAwaitableState::Created);

    io::IoAwaitable<NoPrep> prefailed{ctx, nullptr, /*etime_is_success=*/true, EBADF};
    EXPECT_TRUE(prefailed.etime_is_success());
    EXPECT_TRUE(prefailed.pre_failed());
    EXPECT_EQ(prefailed.res, -EBADF);
    EXPECT_EQ(prefailed.lifecycle(), io::IoAwaitableState::Created);
}

// await_resume() is a pure function of (res, flags, canceling_all_): every
// branch of the mapping is covered, with the exact contexts callers match on.
TEST(IoAwaitableTest, AwaitResumeMapsEveryBranch)
{
    io::Context ctx(0, nullptr, test::test_opts());

    // res >= 0 is a plain success, regardless of flag configuration.
    const auto r_clean = resume_with(ctx, 5, false, false, false);
    ASSERT_TRUE(r_clean.has_value());
    EXPECT_EQ(*r_clean, 5);

    const auto r_all_flags = resume_with(ctx, 5, true, true, true);
    ASSERT_TRUE(r_all_flags.has_value());
    EXPECT_EQ(*r_all_flags, 5);

    // -ETIME is a success value iff etime_is_success (sleep semantics).
    const auto etime_ok = resume_with(ctx, -ETIME, true, false, false);
    ASSERT_TRUE(etime_ok.has_value());
    EXPECT_EQ(*etime_ok, 0);

    const auto etime_err = resume_with(ctx, -ETIME, false, false, false);
    ASSERT_FALSE(etime_err.has_value());
    EXPECT_EQ(etime_err.error().value(), ETIME);
    EXPECT_EQ(etime_err.error().context(), "io operation failed");

    // -ECANCELED from a fired link timeout, no cancel requested anywhere:
    // that IS a timeout.
    const auto timed_out = resume_with(ctx, -ECANCELED, false, true, false);
    ASSERT_FALSE(timed_out.has_value());
    EXPECT_EQ(timed_out.error().value(), ETIMEDOUT);
    EXPECT_EQ(timed_out.error().context(), "operation timed out");

    // ... with cancel_requested: a genuine cancellation.
    const auto canceled = resume_with(ctx, -ECANCELED, false, true, true);
    ASSERT_FALSE(canceled.has_value());
    EXPECT_EQ(canceled.error().value(), ECANCELED);
    EXPECT_EQ(canceled.error().context(), "operation canceled");

    // ... without any link timeout: a genuine cancellation.
    const auto canceled_bare = resume_with(ctx, -ECANCELED, false, false, false);
    ASSERT_FALSE(canceled_bare.has_value());
    EXPECT_EQ(canceled_bare.error().value(), ECANCELED);
    EXPECT_EQ(canceled_bare.error().context(), "operation canceled");

    // ... with the teardown-wide cancel_all() latch set: cancellation wins
    // over the timeout classification.
    {
        io::Context teardown(0, nullptr, test::test_opts());
        teardown.cancel_all();  // latches canceling_all_; the SQE is never submitted here
        const auto canceled_all = resume_with(teardown, -ECANCELED, false, true, false);
        ASSERT_FALSE(canceled_all.has_value());
        EXPECT_EQ(canceled_all.error().value(), ECANCELED);
        EXPECT_EQ(canceled_all.error().context(), "operation canceled");
    }

    // Any other negative res maps to its errno with the generic context.
    const auto io_err = resume_with(ctx, -EIO, false, false, false);
    ASSERT_FALSE(io_err.has_value());
    EXPECT_EQ(io_err.error().value(), EIO);
    EXPECT_EQ(io_err.error().context(), "io operation failed");
}

// One await per op: the state byte leaves Created exactly once, and a
// second manual await_suspend must abort at the bug site.
TEST(IoAwaitableDeathTest, AwaitTwiceTerminates)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    io::UniqueFd recv_side{sv[0]};  // parent copies: closed here, not in the forked child
    io::UniqueFd peer{sv[1]};

    EXPECT_DEATH(
        {
            io::Context ctx(0, nullptr, test::test_opts());
            std::byte b{};
            auto op = async_recv(ctx, io::UniqueFd{sv[0]}, std::span{&b, 1});
            op.await_suspend(std::noop_coroutine());
            op.await_suspend(std::noop_coroutine());
        },
        "awaited twice");
}

// The link timeout is armed at await_suspend; calling timeout() afterwards
// would flag an op whose result is already classified (a real ECANCELED
// would misreport as ETIMEDOUT). The Created-state CHECK turns the misuse
// into a deterministic abort.
TEST(IoAwaitableDeathTest, TimeoutAfterAwaitTerminates)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    io::UniqueFd recv_side{sv[0]};  // parent copies: closed here, not in the forked child
    io::UniqueFd peer{sv[1]};

    EXPECT_DEATH(
        {
            io::Context ctx(0, nullptr, test::test_opts());
            std::byte b{};
            auto op = async_recv(ctx, io::UniqueFd{sv[0]}, std::span{&b, 1});
            op.await_suspend(std::noop_coroutine());   // op is now Inflight
            op.timeout(std::chrono::milliseconds(1));  // too late: CHECK fires
        },
        "after the op was awaited");
}

// A defense for awaiters kio does not own: destroying an op whose CQE is
// still in flight would leave reap() casting a dangling user_data.
TEST(IoAwaitableDeathTest, DestroyedInflightTerminates)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    io::UniqueFd recv_side{sv[0]};
    io::UniqueFd peer{sv[1]};

    EXPECT_DEATH(
        {
            io::Context ctx(0, nullptr, test::test_opts());
            std::byte b{};
            auto op = async_recv(ctx, io::UniqueFd{sv[0]}, std::span{&b, 1});
            op.await_suspend(std::noop_coroutine());
            // Scope end: ~IoAwaitable sees Inflight and aborts before ~Context.
        },
        "destroyed in flight");
}
