#include "kio/baton.h"
#include "kio/context.h"
#include "kio/operations.h"
#include "kio/semaphore.h"
#include "kio/task.h"

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "include/kio/operations.h"
#include "include/kio/task.h"

using namespace kio;

namespace
{
    // Rings here are deliberately tiny (64 entries, epoch cap 8) like the
    // run-loop tests: nothing needs queue depth, and the small cap proves the
    // handoffs survive epoch boundaries.
    //
    // EXPECT (not ASSERT) inside coroutines: ASSERT_* expands to a plain
    // `return`, which is ill-formed in a coroutine body.
    //
    // Death tests spawn INSIDE the EXPECT_DEATH statement: only the forked
    // child then holds the never-run task (the parent's ~Context would
    // otherwise CHECK-abort on spawned-but-never-run work). Same idiom as
    // BufferPoolTest.ForeignLeaseDies / RunDeadlockAborts.

    // ---- Baton task bodies ----

    Task<> baton_waiter(Baton& baton, bool& done)
    {
        co_await baton.wait();
        done = true;
        co_return {};
    }

    Task<> baton_recorder(Baton& baton, const int id, std::vector<int>& order)
    {
        co_await baton.wait();
        order.push_back(id);
        co_return {};
    }

    Task<> delayed_post(io::Context& io, Baton& baton, const std::chrono::milliseconds delay)
    {
        co_await async_sleep(io, delay);
        baton.post();
        co_return {};
    }

    // ---- Semaphore task bodies ----

    Task<> sem_holder(io::Context& io, Semaphore& sem, const std::chrono::milliseconds hold)
    {
        co_await sem.acquire();
        co_await async_sleep(io, hold);
        sem.release();
        co_return {};
    }

    Task<> sem_recorder(Semaphore& sem, const int id, std::vector<int>& order)
    {
        co_await sem.acquire();
        order.push_back(id);
        sem.release();
        co_return {};
    }

    // Takes the only permit, sleeps, then releases mid-task -- the release is
    // what wakes waiter #1; waiter #1's own release() chains the wake to #2,
    // then #3. Right after releasing it probes try_acquire(): the freed permit
    // is already reserved for the woken waiter, so the releaser must NOT be
    // able to take it back (surplus rule).
    Task<> sem_thief(io::Context& io, Semaphore& sem, bool& thief_blocked)
    {
        co_await sem.acquire();
        co_await async_sleep(io, std::chrono::milliseconds(10));
        sem.release();
        thief_blocked = !sem.try_acquire();
        co_return {};
    }

    Task<> sem_taker(Semaphore& sem, bool& ok)
    {
        co_await sem.acquire();
        ok = true;
        co_return {};
    }

    Task<> delayed_release(io::Context& io, Semaphore& sem, const std::chrono::milliseconds delay)
    {
        co_await async_sleep(io, delay);
        sem.release();
        co_return {};
    }
} // namespace

// ---- Baton: one-shot sticky broadcast event ----

// Posting before anyone waits latches ready_, so the late waiter takes the
// await_ready() fast path: no park, no wake, no park-counter traffic.
TEST(BatonTest, PostThenWaitIsFast)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    bool done = false;
    baton.post();
    ctx.spawn(baton_waiter(baton, done));
    ctx.run();

    EXPECT_TRUE(done);
}

// post() is a broadcast: every waiter parked before it wakes, and the
// ReadyRing hands them back in park order (spawn order here).
TEST(BatonTest, BroadcastWakesAllInParkOrder)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    std::vector<int> order;
    ctx.spawn(baton_recorder(baton, 1, order));
    ctx.spawn(baton_recorder(baton, 2, order));
    ctx.spawn(baton_recorder(baton, 3, order));
    ctx.spawn(delayed_post(ctx, baton, std::chrono::milliseconds(10)));
    ctx.run();

    EXPECT_EQ(order, (std::vector{1, 2, 3}));
}

// Same observable outcome as PostThenWaitIsFast, but read as stickiness:
// the baton stays ready after a waiter drained it, so a waiter spawned in
// a LATER run() still completes inline.
TEST(BatonTest, StickyLateWaiterCompletes)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    bool first = false;
    bool second = false;
    baton.post();
    ctx.spawn(baton_waiter(baton, first));
    ctx.run();
    EXPECT_TRUE(first);

    ctx.spawn(baton_waiter(baton, second));
    ctx.run();
    EXPECT_TRUE(second);
}

// Regression pin for the park counter: a parked waiter must keep run()
// alive across the 50ms window -- run() may not return while
// external_parks_ > 0, and the delayed poster is the only thing that can
// clear it.
TEST(BatonTest, QuiescenceWaitsForParkedWaiter)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    bool done = false;
    ctx.spawn(baton_waiter(baton, done)); // parks immediately
    ctx.spawn(delayed_post(ctx, baton, std::chrono::milliseconds(50)));
    ctx.run();

    EXPECT_TRUE(done);
}

// A waiter whose poster never runs is a deadlock run() can prove at exit:
// nothing runnable, nothing in flight, park counter nonzero.
TEST(BatonTest, NeverPostedAborts)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    bool done = false;
    EXPECT_DEATH(
        {
        ctx.spawn(baton_waiter(baton, done));
        ctx.run();
        },
        "never posted");
}

// ---- Semaphore: counting resource, wake-one FIFO ----

// try_acquire serves only the SURPLUS: two of two initial permits go to
// newcomers, the third finds count_ == reserved_ and fails; release()
// without waiters is a pure count bump that re-enables taking.
TEST(SemaphoreTest, TryAcquireSurplusOnly)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 2);

    EXPECT_TRUE(sem.try_acquire());
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire());

    sem.release(); // no waiters: nobody to wake, nobody to reserve for
    EXPECT_TRUE(sem.try_acquire());
}

// Single permit, chained like BufferPoolTest.FifoWakeChainedOrder: the
// holder's release wakes waiter #1, whose own release wakes #2, then #3 --
// completions in spawn order despite capacity 1.
TEST(SemaphoreTest, WakeOneFifoChained)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 1);

    std::vector<int> order;
    ctx.spawn(sem_holder(ctx, sem, std::chrono::milliseconds(10)));
    ctx.spawn(sem_recorder(sem, 1, order));
    ctx.spawn(sem_recorder(sem, 2, order));
    ctx.spawn(sem_recorder(sem, 3, order));
    ctx.run();

    EXPECT_EQ(order, (std::vector{1, 2, 3}));
}

// Mirrors BufferPoolTest.NoBypassSurplusRule: the moment release() pops a
// parked waiter it counts the permit in reserved_, so the releaser's own
// immediate try_acquire() is refused -- the permit is spoken for until the
// woken waiter resumes and consumes its reservation.
TEST(SemaphoreTest, ReleaserCannotStealReservedPermit)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 1);

    bool thief_blocked = false;
    bool waiter_ok = false;
    ctx.spawn(sem_thief(ctx, sem, thief_blocked));
    ctx.spawn(sem_taker(sem, waiter_ok)); // parks while the permit is held
    ctx.run();

    EXPECT_TRUE(thief_blocked);
    EXPECT_TRUE(waiter_ok);
}

// Park-counter counterpart for acquirers: run() must not return while the
// acquirer is parked on an empty semaphore; the delayed release clears it.
TEST(SemaphoreTest, QuiescenceWaitsForParkedAcquirer)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 0);

    bool done = false;
    ctx.spawn(sem_taker(sem, done)); // parks: zero permits
    ctx.spawn(delayed_release(ctx, sem, std::chrono::milliseconds(50)));
    ctx.run();

    EXPECT_TRUE(done);
}

// An acquirer parked on a semaphore nobody ever releases is a deadlock
// run() can prove at exit: nothing runnable, nothing in flight, park
// counter nonzero.
TEST(SemaphoreTest, NeverReleasedAborts)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 0);

    bool done = false;
    EXPECT_DEATH(
        {
        ctx.spawn(sem_taker(sem, done));
        ctx.run();
        },
        "semaphore never released");
}

// Baton/Semaphore are built per locked resource / per bridge (the Mutex and
// one-shot-channel use cases in baton.md), so their waiter queue must stay
// lazy: an eager 1024-slot ring would be 8 KiB + one malloc per instance.
// The parking path pays one allocation on first park; the fast path none.
TEST(BatonTest, PrimitiveFootprintStaysSmall)
{
    EXPECT_LE(sizeof(Baton), 24u);
    EXPECT_LE(sizeof(Semaphore), 32u);
}

// ===========================================================================
// Baton: extended pins
// ===========================================================================

namespace
{
    // Manual-drive bodies: the caller parks/resumes the task by hand, so
    // interleavings the run loop cannot produce become observable.
    Task<> baton_parker(Baton& baton)
    {
        co_await baton.wait();
        co_return {};
    }
} // namespace

// post() is idempotent: the second call finds an empty waiter ring and a
// ready_ already latched. Late waiters keep completing inline.
TEST(BatonTest, PostTwiceIsANoop)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    baton.post();
    EXPECT_TRUE(baton.try_wait());
    baton.post(); // must be harmless

    bool done = false;
    ctx.spawn(baton_waiter(baton, done));
    ctx.run();
    EXPECT_TRUE(done);
}

// A newcomer arriving BETWEEN the wake and the waiter's resume sees the
// sticky ready_ and completes inline -- it never parks and cannot steal
// anything (contrast Semaphore; baton.h Invariants).
TEST(BatonTest, NewcomerBetweenWakeAndResumeCompletesInline)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    auto first = baton_parker(baton);
    first.await_suspend(std::noop_coroutine()).resume(); // parks
    EXPECT_EQ(baton.waiting(), 1u);

    baton.post(); // first is now woken: its handle sits in the ready list
    EXPECT_EQ(baton.waiting(), 0u);

    bool late_done = false;
    ctx.spawn(baton_waiter(baton, late_done)); // completes inline during run
    ctx.run(); // resumes the first waiter and the newcomer

    EXPECT_TRUE(late_done);
    EXPECT_TRUE(baton.try_wait());
}

// waiting() mirrors the parking lifecycle: 1 while parked, 0 once the wake
// has popped the handle (at wake time, not at resume time).
TEST(BatonTest, WaitingAccessorTracksTheParkLifecycle)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    EXPECT_EQ(baton.waiting(), 0u); // ring not even allocated yet

    auto t = baton_parker(baton);
    t.await_suspend(std::noop_coroutine()).resume();
    EXPECT_EQ(baton.waiting(), 1u);

    baton.post();
    EXPECT_EQ(baton.waiting(), 0u); // popped from the ring at wake time

    ctx.run();
}

// 100 waiters force the lazy waiter ring past its initial 16-slot capacity
// while parked; post() must still wake all of them, in park order.
TEST(BatonTest, HundredWaitersWakeFifoThroughRingGrowth)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Baton baton(ctx);

    std::vector<int> order;
    order.reserve(100);
    for (int i = 0; i < 100; ++i)
    {
        ctx.spawn(baton_recorder(baton, i, order));
    }
    ctx.spawn(delayed_post(ctx, baton, std::chrono::milliseconds(20)));
    ctx.run();

    ASSERT_EQ(static_cast<int>(order.size()), 100);
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(order[static_cast<size_t>(i)], i);
    }
}

// Destroying a Baton with handles still parked would dangle them: the
// destructor CHECK-aborts naming the culprit.
TEST(BatonDeathTest, DestroyedWithParkedWaitersTerminates)
{
    io::IoOptions opts;
    opts.entries = 64;
    EXPECT_DEATH(
        {
        io::Context ctx(0, nullptr, opts);
        auto baton = std::make_unique<Baton>(ctx);
        auto t = baton_parker(*baton);
        t.await_suspend(std::noop_coroutine()).resume();
        baton.reset(); // ~Baton sees a non-empty waiter ring
        },
        "parked waiters");
}

// ===========================================================================
// Semaphore: extended pins
// ===========================================================================

TEST(SemaphoreTest, NPlusOnethTryAcquireFails)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 3);

    EXPECT_TRUE(sem.try_acquire());
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire()); // the N+1th
    EXPECT_EQ(sem.available(), 0u);
}

// release() past the initial count is allowed: the count is a floor, not a
// ceiling (same latitude as POSIX semaphores).
TEST(SemaphoreTest, OverReleaseRaisesTheFloor)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 1);

    sem.release(); // no waiters: pure count bumps
    sem.release();
    EXPECT_EQ(sem.available(), 3u);

    EXPECT_TRUE(sem.try_acquire());
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire());
}

// A newcomer arriving after a release (permit already reserved for a woken
// waiter) finds no surplus and parks behind it.
TEST(SemaphoreTest, NewcomerParksBehindAReservedPermit)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 0);

    bool first_ok = false;
    auto first = sem_taker(sem, first_ok);
    first.await_suspend(std::noop_coroutine()).resume(); // parks: zero permits
    EXPECT_EQ(sem.waiting(), 1u);

    sem.release(); // permit reserved for the woken first waiter
    EXPECT_EQ(sem.waiting(), 0u);
    EXPECT_FALSE(sem.try_acquire()); // reserved: no surplus

    bool second_ok = false;
    auto second = sem_taker(sem, second_ok);
    second.await_suspend(std::noop_coroutine()).resume(); // parks behind the reservation
    EXPECT_EQ(sem.waiting(), 1u);

    sem.release(); // second release: wakes the newcomer
    EXPECT_EQ(sem.waiting(), 0u);

    // Both reservations are now consumed: drive the rest to completion.
    ctx.run();
    EXPECT_TRUE(first_ok);
    EXPECT_TRUE(second_ok);
}

// Two releases before either waiter resumes: both wake in order, each
// consumes its own reservation, and the end state is zero available.
TEST(SemaphoreTest, TwoReleasesWakeTwoWaitersEachConsumesItsOwn)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 0);

    bool a_ok = false;
    bool b_ok = false;
    auto a = sem_taker(sem, a_ok);
    auto b = sem_taker(sem, b_ok);
    a.await_suspend(std::noop_coroutine()).resume();
    b.await_suspend(std::noop_coroutine()).resume();
    ASSERT_EQ(sem.waiting(), 2u);

    sem.release();
    sem.release(); // both woken, both reserved -- nobody has resumed yet
    EXPECT_EQ(sem.available(), 2u);
    EXPECT_EQ(sem.waiting(), 0u);

    // The woken handles sit in the Context's ready list; run() resumes both,
    // each consuming its own reservation.
    ctx.run();

    EXPECT_TRUE(a_ok);
    EXPECT_TRUE(b_ok);
    EXPECT_EQ(sem.available(), 0u);
}

// Mutual exclusion under contention: 50 tasks share 3 permits; the
// in-critical-section counter must never exceed 3 and every task finishes.
TEST(SemaphoreTest, MutualExclusionCapsConcurrencyAtThree)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 3);

    int current = 0;
    int max_seen = 0;
    int finished = 0;

    auto contender = [&sem, &current, &max_seen, &finished](io::Context& io) -> Task<>
    {
        co_await sem.acquire();
        ++current;
        max_seen = std::max(max_seen, current);
        co_await async_sleep(io, std::chrono::milliseconds(1));
        --current;
        sem.release();
        ++finished;
        co_return {};
    };
    for (int i = 0; i < 50; ++i)
    {
        ctx.spawn(contender(ctx));
    }
    ctx.run();

    EXPECT_EQ(max_seen, 3);
    EXPECT_EQ(finished, 50);
    EXPECT_EQ(sem.available(), 3u);
}

// Counting fuzz: 200k random acquire/release steps against a trivial count
// model. try_acquire must succeed exactly when the model has permits, and
// available() must always equal the model. (The reservation interleavings
// are pinned by the targeted tests above; this fuzz catches counting drift
// across thousands of transitions.)
TEST(SemaphoreTest, ConservationFuzzAgainstCountModel)
{
    io::IoOptions opts;
    opts.entries = 64;
    opts.batch_max_size = 8;
    io::Context ctx(0, nullptr, opts);
    Semaphore sem(ctx, 8);

    std::mt19937_64 rng(20261014u);
    long model = 8;

    for (int step = 0; step < 200'000; ++step)
    {
        if (rng() % 3 == 0)
        {
            sem.release();
            ++model;
        }
        else
        {
            const bool got = sem.try_acquire();
            EXPECT_EQ(got, model > 0) << "step " << step;
            if (got)
            {
                --model;
            }
        }
        EXPECT_EQ(static_cast<long>(sem.available()), model) << "step " << step;
    }
}
