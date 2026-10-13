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

// Setters must not disturb unrelated bits; set_lifecycle must preserve the
// flags; lifecycle() must be immune to the flag bits. The full 3-lifecycle x
// 2^4-flag space is walked through the public API.
TEST(IoAwaitableTest, LifecycleAndFlagBitsPackIntoOneByte)
{
    io::Context ctx(0, nullptr, test::test_opts());

    for (const bool etime : {false, true})
    {
        for (const bool prefailed : {false, true})
        {
            io::IoAwaitable<NoPrep> op{ctx, nullptr, etime, prefailed ? 1 : 0};
            EXPECT_EQ(op.pre_failed(), prefailed);
            EXPECT_EQ(op.res, prefailed ? -1 : 0);  // ctor seeds res for a pre-failed op

            for (const bool link : {false, true})
            {
                for (const bool cancel : {false, true})
                {
                    if (link)
                    {
                        op.set_has_link_timeout();
                    }
                    else
                    {
                        op.state &= static_cast<std::uint8_t>(~io::kHasLinkTimeoutBit);
                    }
                    if (cancel)
                    {
                        op.set_cancel_requested();
                    }
                    else
                    {
                        op.state &= static_cast<std::uint8_t>(~io::kCancelRequestedBit);
                    }

                    for (const auto life :
                         {io::IoAwaitableState::Created, io::IoAwaitableState::Inflight, io::IoAwaitableState::Done})
                    {
                        op.set_lifecycle(life);
                        EXPECT_EQ(op.lifecycle(), life);
                        EXPECT_EQ(op.etime_is_success(), etime);
                        EXPECT_EQ(op.has_link_timeout(), link);
                        EXPECT_EQ(op.cancel_requested(), cancel);
                        EXPECT_EQ(op.pre_failed(), prefailed);
                    }
                }
            }
        }
    }
}

// await_resume() is a pure function of (res, flags, canceling_all_): every
// branch of the mapping is covered, with the exact contexts callers match on.
TEST(IoAwaitableTest, AwaitResumeMapsEveryBranch)
{
    io::Context ctx(0, nullptr, test::test_opts());

    // res >= 0 is a plain success under every flag combination.
    for (const bool etime : {false, true})
    {
        for (const bool link : {false, true})
        {
            for (const bool cancel : {false, true})
            {
                const auto r = resume_with(ctx, 5, etime, link, cancel);
                EXPECT_TRUE(r.has_value());
                EXPECT_EQ(*r, 5);
            }
        }
    }

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
