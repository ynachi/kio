#include "kio/baton.h"
#include "kio/context.h"
#include "kio/operations.h"
#include "kio/semaphore.h"
#include "kio/task.h"

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

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
}  // namespace

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

    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
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
    ctx.spawn(baton_waiter(baton, done));  // parks immediately
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

    sem.release();  // no waiters: nobody to wake, nobody to reserve for
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

    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
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
    ctx.spawn(sem_taker(sem, waiter_ok));  // parks while the permit is held
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
    ctx.spawn(sem_taker(sem, done));  // parks: zero permits
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
