#include "kio/context.h"
#include "kio/operations.h"
#include "kio/task.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <thread>

#include <unistd.h>

#include <sys/socket.h>

#include "test_util.hpp"
#include <gtest/gtest.h>

using namespace kio;

namespace
{
// Touched only from the loop thread (Context::run), so no synchronization.
int g_completed = 0;

Task<> counting_noop()
{
    ++g_completed;
    co_return {};
}
}  // namespace

// A backlog larger than the epoch cap must still terminate: each capped epoch
// drains 8 resumes, then takes the non-blocking Phase 2 path (io_uring_submit
// on an empty SQ is a ring check, not a syscall) until the queue empties.
TEST(RunLoopTest, BacklogDrainsAcrossCappedEpochs)
{
    g_completed = 0;
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    for (int i = 0; i < 500; ++i)
    {
        ctx.spawn(counting_noop());
    }
    ctx.run();
    EXPECT_EQ(g_completed, 500);
}

TEST(RunLoopTest, CapOfOneStillCompletes)
{
    g_completed = 0;
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 1;
    io::Context ctx(0, nullptr, opts);
    for (int i = 0; i < 50; ++i)
    {
        ctx.spawn(counting_noop());
    }
    ctx.run();
    EXPECT_EQ(g_completed, 50);
}

TEST(RunLoopTest, LargeCapDegradesToDrainAll)
{
    g_completed = 0;
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 4096;
    io::Context ctx(0, nullptr, opts);
    for (int i = 0; i < 1000; ++i)
    {
        ctx.spawn(counting_noop());
    }
    ctx.run();
    EXPECT_EQ(g_completed, 1000);
}

// ---- DEFER_TASKRUN starvation canary (regression test) ----
//
// With DEFER_TASKRUN, completions finished outside submit (task work) are
// only posted when the loop enters the kernel WITH GETEVENTS. A busy,
// self-refilling ready list takes the non-blocking Phase 2 branch every
// epoch; with the old plain io_uring_submit there, it never asked for
// events (and an empty-SQ submit does not enter the kernel at all), so
// nothing could complete while the list stayed busy. The busy chain below
// uses a WALL clock, not an io_uring timer, because timers starve the same
// way. Discriminator: recv_done must be true DURING the storm (checked at
// wind-down), not only after the chain stops and the loop blocks.
namespace
{
bool g_busy_stop = false;

Task<> recv_one_byte(io::Context& io, io::UniqueFd fd, bool& done)
{
    std::byte b{};
    auto n = co_await async_recv(io, fd, std::span{&b, 1});
    done = n.has_value() && *n == 1;
    co_return {};
}

Task<> write_one_byte_after(io::Context& io, io::UniqueFd fd)
{
    co_await async_sleep(io, std::chrono::milliseconds(10));
    const char c = 'x';
    (void)::write(fd.get(), &c, 1);  // completes the recv via deferred task work
    co_return {};
}

Task<> busy_chain(io::Context& io, bool& recv_done, bool& ok, const std::chrono::steady_clock::time_point deadline)
{
    if (g_busy_stop)
    {
        ok = recv_done;  // verdict at wind-down: completed DURING the storm?
        co_return {};
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
        g_busy_stop = true;
    }
    io.spawn(busy_chain(io, recv_done, ok, deadline));  // keep ready_list_ busy
    co_return {};
}
}  // namespace

TEST(RunLoopTest, BusyReadyListDoesNotStarveCompletions)
{
    g_busy_stop = false;
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);

    bool recv_done = false;
    bool ok = false;
    ctx.spawn(recv_one_byte(ctx, io::UniqueFd{sv[0]}, recv_done));
    ctx.spawn(write_one_byte_after(ctx, io::UniqueFd{sv[1]}));
    ctx.spawn(busy_chain(ctx, recv_done, ok, std::chrono::steady_clock::now() + std::chrono::milliseconds(500)));

    ctx.run();

    // Completes after the chain stops even with the bug (the loop then
    // blocks in submit_and_wait, which runs task work) -- the real
    // assertion is `ok`: completion had to happen mid-storm.
    EXPECT_TRUE(recv_done);
    EXPECT_TRUE(ok);
}

// ===========================================================================
// Ordering, lifecycle, and construction validation
// ===========================================================================

// FIFO must hold ACROSS epoch boundaries: with cap 4 and 20 tasks, the
// ready ring is drained in 4-resume chunks, yet completion order is spawn
// order 0..19 (the Phase 1 stack-copy contract in ready-ring.md).
TEST(RunLoopTest, FifoCompletionOrderAcrossEpochCaps)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 4;
    io::Context ctx(0, nullptr, opts);

    // One recorder per id: resume time is completion time, and the epoch
    // cap forces the 20 resumes through five capped chunks.
    std::vector<int> recorded;
    recorded.reserve(20);
    auto recorder = [&recorded](const int id) -> Task<>
    {
        recorded.push_back(id);
        co_return {};
    };
    for (int i = 0; i < 20; ++i)
    {
        ctx.spawn(recorder(i));
    }
    ctx.run();

    ASSERT_EQ(recorded.size(), 20u);
    for (int i = 0; i < 20; ++i)
    {
        EXPECT_EQ(recorded[static_cast<size_t>(i)], i);
    }
}

TEST(RunLoopTest, RunTwiceIsLegal)
{
    g_completed = 0;
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts);

    ctx.spawn(counting_noop());
    ctx.run();
    EXPECT_EQ(g_completed, 1);

    ctx.spawn(counting_noop());
    ctx.run();  // second session on the same Context
    EXPECT_EQ(g_completed, 2);
}

TEST(RunLoopDeathTest, BatchMaxSizeZeroAbortsAtConstruction)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 0;
    EXPECT_DEATH({ io::Context ctx(0, nullptr, opts); }, "batch_max_size must be positive");
}

TEST(RunLoopDeathTest, DestroyWithUnstartedSpawnAborts)
{
    EXPECT_DEATH(
        {
            io::IoOptions opts;
            opts.entries = 64;
            io::Context ctx(0, nullptr, opts);
            ctx.spawn(counting_noop());  // never run: ~Context must abort
        },
        "unstarted work");
}

// BufferPoolConfig.slots == 0 passes the Context's slot_size CHECK and is
// rejected by the pool constructor: invalid_argument, and the failed
// construction must not leak the ring fd (the pool is built before the
// ring is created).
TEST(RunLoopTest, ZeroSlotPoolThrowsWithoutFdLeak)
{
    io::IoOptions opts;
    opts.entries = 64;
    const auto before = test::OpenFdCount();
    bool threw = false;
    try
    {
        io::Context ctx(0, nullptr, opts, BufferPoolConfig{.slot_size = 4096, .slots = 0});
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(test::OpenFdCount(), before);
}

// ---- loop-thread contract, spawn-before-run shape ----
//
// The review's §3.1: init()'s R_DISABLED comment says "construct + spawn on
// A, run() on B" is a supported pattern, and check_loop_thread() now agrees
// -- it exempts everything before the first run(), because run() activates
// before it binds the owner. This test drives exactly that pattern on a
// debug build (where the CHECK exists and would fire if the exemption were
// ever dropped again); the enforcement side stays pinned by
// TimingCancelTest.CrossThreadSpawnAbortsInDebug (run first, then a
// cross-thread spawn aborts).
TEST(RunLoopTest, SpawnBeforeRunThenRunOnAnotherThreadIsLegal)
{
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts);

    int v = 0;
    auto touch = [](int& target) -> Task<>
    {
        ++target;
        co_return {};
    };
    ctx.spawn(touch(v));  // pre-activation post(): no owner binding yet

    std::thread t(
        [&ctx]
        {
            ctx.run();  // binds the loop-thread identity to THIS thread
        });
    t.join();

    EXPECT_EQ(v, 1);
}

// ===========================================================================
// Small-ring stress: the full-SQ submit path in get_sqe
// ===========================================================================

// 200 concurrent pipe writes on an 8-entry ring: get_sqe must drain and
// resubmit whenever the SQ runs out of space, losing nothing.
TEST(RunLoopTest, FullSubmissionQueueStressWithTinyRing)
{
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const io::UniqueFd r{fds[0]};
    const io::UniqueFd w{fds[1]};

    io::IoOptions opts;
    opts.entries = 8;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);

    int ok_writes = 0;
    auto writer = [&ok_writes](io::Context& io, const io::UniqueFd& wfd) -> Task<>
    {
        const std::byte payload[16]{};
        auto n = co_await async_write(io, wfd, std::span{payload, 16});
        if (n.has_value() && *n == 16)
        {
            ++ok_writes;
        }
        co_return {};
    };
    for (int i = 0; i < 200; ++i)
    {
        ctx.spawn(writer(ctx, w));
    }
    ctx.run();

    EXPECT_EQ(ok_writes, 200);
}

// 100 linked-timeout ops on the same 8-entry ring: each op reserves TWO
// SQEs (primary + link timeout); the 2-SQE reservation must never split a
// link across a drained-and-resubmitted SQ.
TEST(RunLoopTest, LinkTimeoutBatchNeverSplitsOnTinyRing)
{
    io::IoOptions opts;
    opts.entries = 8;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);

    int ok_sleeps = 0;
    auto sleeper = [&ok_sleeps](io::Context& io) -> Task<>
    {
        auto r = co_await async_sleep(io, std::chrono::milliseconds(5)).timeout(std::chrono::seconds(1));
        if (r.has_value() && *r == 0)
        {
            ++ok_sleeps;
        }
        co_return {};
    };
    for (int i = 0; i < 100; ++i)
    {
        ctx.spawn(sleeper(ctx));
    }
    ctx.run();

    EXPECT_EQ(ok_sleeps, 100);
}

// ===========================================================================
// Stop semantics: the one force-drain mode (design-notes/shutdown.md)
// ===========================================================================
//
// The token is the force channel ONLY: read at the loop's arming check, it
// arms cancel_all() + fail-fast staging, and run() still returns only when
// quiesced. The tick constant (50ms) and the -ETIME convention of
// io_uring_submit_and_wait_timeout are pinned behaviorally by these tests.

namespace
{
// Drives ctx.run(st) on a worker thread and waits up to `budget` for it to
// return. A regression that hangs run() fires the caller's failing EXPECT
// first, then blocks in join -- the hang IS the loud signal.
bool run_with_stop(io::Context& ctx, std::stop_token st, const std::chrono::milliseconds budget)
{
    std::atomic<bool> returned{false};
    std::jthread driver(
        [&ctx, st = std::move(st), &returned]
        {
            ctx.run(st);
            returned = true;
        });
    for (auto start = std::chrono::steady_clock::now();
         !returned.load() && std::chrono::steady_clock::now() - start < budget;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool ok = returned.load();
    driver.join();
    return ok;
}

Task<> recv_hold_forever(io::Context& io, io::UniqueFd fd, Result<int>& out)
{
    std::byte b{};
    out = co_await async_recv(io, fd, std::span{&b, 1});
    co_return {};
}
}  // namespace

// The old loop's `while (!st.stop_requested())` hung forever when the
// request landed with everything already drained: the stop-suppressed
// quiesce exit fell into submit_and_wait for a CQE nothing could produce.
// A task whose LAST act is request_stop is the deterministic repro.
TEST(RunLoopTest, StopAsLastActStillReturns)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    std::stop_source src;
    bool ran = false;
    auto stopper = [&src, &ran]() -> Task<>
    {
        ran = true;
        src.request_stop();
        co_return {};
    };
    ctx.spawn(stopper());

    ctx.run(src.get_token());

    EXPECT_TRUE(ran);
}

// Stop landing while the loop is BLOCKED on a pending op: the deadline tick
// is the only sanctioned wake. A mis-handled -ETIME aborts in the FATAL
// check; an unbounded wait hangs. Either way this test fails loudly.
TEST(RunLoopTest, StopDuringBlockedWaitDrainsBounded)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    std::stop_source src;
    Result<int> out;
    ctx.spawn(recv_hold_forever(ctx, io::UniqueFd{sv[0]}, out));

    EXPECT_TRUE(run_with_stop(ctx, src.get_token(), std::chrono::seconds(5)));

    // The parked recv was force-cancelled by the drain, not left hanging.
    EXPECT_EQ(out.error().value(), ECANCELED);
    EXPECT_EQ(out.error().context(), "operation canceled");
}

// A task that retries forever on success must not defeat the drain: the
// fail-fast latch makes inflight_ monotone, so the retry's next await
// completes inline with -ECANCELED (no SQE ever staged again) and the loop
// exits. A one-shot cancel + re-stage design livelocks here instead.
TEST(RunLoopTest, RetryLoopDrainsInsteadOfLivelocking)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    std::stop_source src;
    int retries = 0;
    bool canceled = false;
    auto retrier = [&ctx, &retries, &canceled]() -> Task<>
    {
        for (;;)
        {
            auto r = co_await async_sleep(ctx, std::chrono::milliseconds(5));
            if (!r)
            {
                canceled = r.error().value() == ECANCELED;
                co_return {};
            }
            ++retries;
        }
    };
    ctx.spawn(retrier());

    EXPECT_TRUE(run_with_stop(ctx, src.get_token(), std::chrono::seconds(5)));

    EXPECT_GE(retries, 5);  // it really was looping before the stop
    EXPECT_TRUE(canceled);
}

// Row 3 of the token state machine: destroying every stop_source WITHOUT
// requesting leaves stop_possible() == false -- the loop becomes
// un-stoppable (nobody can ever signal it). Correct, but the opposite of
// an ownership-transfer intuition; the dead-source session still quiesces.
TEST(RunLoopTest, SourceDestroyedWithoutRequestStillQuiesces)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    std::stop_token st;
    {
        std::stop_source src;
        st = src.get_token();
    }
    EXPECT_FALSE(st.stop_possible());
    EXPECT_FALSE(st.stop_requested());

    g_completed = 0;
    ctx.spawn(counting_noop());
    ctx.run(st);

    EXPECT_EQ(g_completed, 1);
}
