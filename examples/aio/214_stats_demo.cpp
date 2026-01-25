#include <atomic>
#include <chrono>
#include <print>
#include <thread>
#include <vector>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/stats.hpp"
#include "aio/task_group.hpp"

// Ensure stats are enabled
#if !AIO_STATS
#error "AIO_STATS must be enabled in build configuration (CMake) for this demo to work."
#endif

using namespace aio;
using namespace std::chrono_literals;

void PrintStats(const IoContextStats::Snapshot& s)
{
    // Clear screen or just print
    std::print("\033[2J\033[H"); // ANSI clear screen
    std::println("=== AIO Metrics ===");
    std::println("  Ops Submitted:   {}", s.ops_submitted);
    std::println("  Ops Completed:   {}", s.ops_completed);
    std::println("  Ops Errors:      {}", s.ops_errors);
    std::println("  Ops In-Flight:   {}", s.ops_inflight);
    std::println("  Max In-Flight:   {}", s.ops_max_inflight);
    std::println("  Timeouts:        {}", s.timeouts);
    std::println("-------------------");
    std::println("  Loop Iterations: {}", s.loop_iterations);
    std::println("  Busy Iterations: {}", s.loop_busy_iterations);
    std::println("  Idle Iterations: {}", s.loop_idle_iterations);
    std::println("  Wakeups:         {}", s.loop_wakeups);
    std::println("  Completions:     {}", s.loop_completions);
    std::println("  Max Batch Size:  {}", s.loop_max_batch);
    std::println("  External Comps:  {}", s.external_completions);
}

// A simple task that sleeps periodically to generate I/O load
Task<> Ticker(IoContext& ctx, int id, std::chrono::milliseconds interval, int count)
{
    for (int i = 0; i < count; ++i)
    {
        co_await AsyncSleep(ctx, interval);
    }
    std::println("ticker {} done", id);
}

// Main demo task that spawns workers and waits for them
Task<> RunDemo(IoContext& ctx)
{
    std::println("Starting Tickers...");

    // We MUST use TaskGroup to run tasks in parallel.
    // Manually calling Start() and then co_await'ing a Task is unsafe
    // because co_await attempts to resume the task again, causing Undefined Behavior
    // if it is already suspended on I/O.
    TaskGroup<> group(2);

    // Spawn tasks into the group (starts them immediately)
    group.Spawn(Ticker(ctx, 1, 10ms, 200)); // ~2 seconds
    group.Spawn(Ticker(ctx, 2, 20ms, 100)); // ~2 seconds

    // Wait for all tasks in the group to finish
    co_await group.JoinAll(ctx);

    std::println("Tickers finished.");
}

int main()
{
    try
    {
        IoContext ctx;
        std::atomic<bool> running{true};

        // Printer thread to show stats while the loop runs
        std::jthread printer([&]
        {
            while (running.load(std::memory_order_relaxed))
            {
                PrintStats(ctx.Stats().GetSnapshot());
                std::this_thread::sleep_for(250ms);
            }
            // Final print
            PrintStats(ctx.Stats().GetSnapshot());
        });

        // Run the main task until it finishes
        auto main_task = RunDemo(ctx);
        ctx.RunUntilDone(main_task);

        // Stop printer
        running.store(false, std::memory_order_relaxed);
    }
    catch (const std::exception& e)
    {
        std::println(stderr, "Fatal: {}", e.what());
        return 1;
    }
    return 0;
}