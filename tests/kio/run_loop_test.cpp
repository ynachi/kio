#include "kio/context.h"
#include "kio/operations.h"
#include "kio/task.h"

#include <chrono>
#include <cstddef>

#include <unistd.h>

#include <sys/socket.h>

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
