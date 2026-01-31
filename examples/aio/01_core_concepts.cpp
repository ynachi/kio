// 01_core_concepts.cpp
//
// The Foundation:
// 1. Initializing the IoContext (the event loop).
// 2. Running concurrent tasks (coroutines).
// 3. Handling time (AsyncSleep).
// 4. Graceful shutdown via SignalSet.

#include <chrono>
#include <csignal>  // Required for signal blocking
#include <print>

#include "aio/aio.hpp"
#include "aio/logger.hpp"

using namespace std::chrono_literals;
using aio::IoContext;
using aio::Task;

namespace
{

// Helper: Block signals in the main thread so they are handled
// asynchronously by SignalSet (via signalfd) rather than the OS default handler.
void BlockSignals()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
}

// We pass a stop flag reference to control the lifecycle of the agents.
// This is "Cooperative Cancellation": the task decides when it is safe to stop.
Task<> Agent(IoContext& ctx, int id, std::chrono::microseconds interval, const bool& stop_token)
{
    ALOG_INFO("[Agent {}] Started (interval: {}us)", id, interval.count());
    int tick = 0;

    while (!stop_token)
    {
        co_await aio::AsyncSleep(ctx, interval);

        // Check again after waking up; we might need to stop immediately.
        if (stop_token)
            break;
        ++tick;
        ALOG_INFO("[Agent {}] Tick {}", id, tick);
    }
    ALOG_INFO("[Agent {}] Shutting down", id);
}

Task<> MainTask(IoContext& ctx)
{
    // Shared flag to signal shutdown to all agents
    bool stop_agents = false;

    // TaskGroup allows running multiple tasks concurrently on the single-threaded context
    auto agents = aio::TaskGroup();

    // Pass the stop flag to all agents
    auto a1 = Agent(ctx, 1, 500ms, stop_agents);
    auto a2 = Agent(ctx, 2, 1000ms, stop_agents);
    auto a3 = Agent(ctx, 3, 1500ms, stop_agents);

    agents.SpawnAll(std::move(a1), std::move(a2), std::move(a3));

    // Wait for a termination signal (Ctrl+C).
    const aio::SignalSet signals{SIGINT, SIGTERM};
    ALOG_INFO("System running. Press Ctrl+C to stop.");

    // This suspends MainTask until a signal arrives
    auto sig = co_await aio::AsyncWaitSignal(ctx, signals.fd());
    ALOG_WARN("\nReceived Signal {}. Shutting down...", *sig);

    // 1. Signal agents to stop looping (Cooperative Request)
    stop_agents = true;

    // 2. Wait for all agents to finish their last iteration (Graceful Wait)
    co_await agents.JoinAll(ctx);

    ALOG_INFO("All agents successfully stopped");
}

}  // namespace

int main()
{
    // 0. Logging setup
    aio::alog::g_level = aio::alog::Level::Info;

    // 1. Block signals (Boilerplate hidden in helper)
    BlockSignals();

    // 2. Create the Context. This owns the io_uring instance.
    IoContext ctx;

    // 3. Run the main task until it completes.
    ctx.RunUntilDone(MainTask(ctx));

    return 0;
}