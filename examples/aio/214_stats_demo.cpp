#include <atomic>
#include <chrono>
#include <print>
#include <thread>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/stats.hpp"

using namespace aio;
using namespace std::chrono_literals;

#define AIO_STATS


void PrintStats(const IoContextStats::Snapshot& s)
{
    std::println("ops submitted: {} completed: {} errors: {} inflight: {} max: {} timeouts: {}",
                 s.ops_submitted, s.ops_completed, s.ops_errors, s.ops_inflight, s.ops_max_inflight, s.timeouts);
    std::println("loop iters: {} busy: {} idle: {} wakeups: {} completions: {} max batch: {} external: {}",
                 s.loop_iterations, s.loop_busy_iterations, s.loop_idle_iterations, s.loop_wakeups, s.loop_completions,
                 s.loop_max_batch, s.external_completions);
}

Task<> Ticker(IoContext& ctx, int id, std::chrono::milliseconds interval, int count)
{
    for (int i = 0; i < count; ++i)
    {
        co_await AsyncSleep(ctx, interval);
    }
    std::println("ticker {} done", id);
}

Task<> RunDemo(IoContext& ctx)
{
    auto t1 = Ticker(ctx, 1, 10ms, 200);
    auto t2 = Ticker(ctx, 2, 20ms, 100);

    t1.Start();
    t2.Start();

    co_await t1;
    co_await t2;

    ctx.Stop();
}

int main()
{
    try
    {
        IoContext ctx;
        std::atomic<bool> running{true};

        std::jthread printer([&]
                             {
                                 while (running.load(std::memory_order_relaxed))
                                 {
                                     PrintStats(ctx.Stats().GetSnapshot());
                                     std::this_thread::sleep_for(500ms);
                                 }
                             });

        auto t = RunDemo(ctx);
        t.Start();
        ctx.Run();

        running.store(false, std::memory_order_relaxed);
    }
    catch (const std::exception& e)
    {
        std::println(stderr, "Fatal: {}", e.what());
        return 1;
    }
    return 0;
}

