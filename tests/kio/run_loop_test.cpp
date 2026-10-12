#include "kio/context.h"
#include "kio/task.h"

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
