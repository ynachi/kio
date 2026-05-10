#include "uring/context.h"

#include <csignal>
#include <cstdio>
#include <stop_token>

#include "uring/io.hpp"
#include "uring/task.hpp"
#include "uring/tracer.hpp"

using namespace URing;

// Writes "hello\n" to stdout, reads it back from /dev/null (always 0 bytes),
// then sleeps 50ms — just enough to exercise Submit → Complete → Wake.
Task<void> tracer_smoke_test(IoContext& ctx, std::stop_source& ss)
{
    // 1. open /dev/null for reading
    auto fd_res = co_await URing::open(ctx, "/dev/null", O_RDONLY);
    if (!fd_res)
    {
        std::fprintf(stderr, "open failed: %s\n", fd_res.error().message().c_str());
        ss.request_stop();
        co_return;
    }
    Fd& fd = *fd_res;

    // 2. read (will return 0 bytes — fine, we just want the trace entries)
    std::byte buf[64]{};
    auto read_res = co_await URing::read(ctx, fd, buf);
    std::fprintf(stdout, "read returned: %d bytes\n", read_res.value_or(-1));

    // 3. sleep 50ms — gives us a Submit + Complete pair with a visible timestamp gap
    co_await URing::sleep(ctx, std::chrono::milliseconds(50));
    std::fprintf(stdout, "sleep done\n");
    ss.request_stop();
}

int main()
{
    // Flush tracer on Ctrl-C
    std::signal(SIGINT,
                [](int)
                {
                    Tracer::flush();
                    std::exit(0);
                });

    IoContext ctx;
    std::stop_source ss;

    // Keep the entire io_uring lifecycle on one thread: construct the task,
    // first-resume it, and run the reactor from the same owner thread.
    auto task = tracer_smoke_test(ctx, ss);
    task.handle_.resume();  // initial_suspend = suspend_always, so we kick it once

    ctx.run(ss.get_token(), std::chrono::milliseconds(10));

    // Dump everything
    Tracer::flush();
    return 0;
}
