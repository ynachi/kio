// examples/06_graceful_shutdown.cpp
// Demonstrates: SignalSet, AsyncWaitSignal, graceful termination

#include <chrono>
#include <print>

#include <csignal>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/task.hpp"

using namespace std::chrono_literals;

aio::Task<> BackgroundWork(aio::IoContext& ctx, int id, std::atomic<bool>& running)
{
    while (running.load(std::memory_order_relaxed))
    {
        std::println("[Worker {}] Doing work...", id);
        co_await aio::AsyncSleep(ctx, 1s);
    }
    std::println("[Worker {}] Shutting down", id);
}

aio::Task<> SignalWaiter(aio::IoContext& ctx, std::atomic<bool>& running)
{
    // Block SIGINT and SIGTERM, handle them via signalfd
    aio::SignalSet signals{SIGINT, SIGTERM};

    std::println("Press Ctrl+C to initiate shutdown...\n");

    auto result = co_await aio::AsyncWaitSignal(ctx, signals.fd());
    if (result)
    {
        const char* name = (*result == SIGINT) ? "SIGINT" : "SIGTERM";
        std::println("\nReceived {}, initiating graceful shutdown...", name);
    }

    running.store(false, std::memory_order_relaxed);
}

aio::Task<> AsyncMain(aio::IoContext& ctx)
{
    std::atomic<bool> running{true};

    // Start background workers
    auto worker1 = BackgroundWork(ctx, 1, running);
    auto worker2 = BackgroundWork(ctx, 2, running);
    worker1.Start();
    worker2.Start();

    // Wait for shutdown signal
    co_await SignalWaiter(ctx, running);

    // Give workers time to notice the flag (in real code, use proper sync)
    co_await aio::AsyncSleep(ctx, 100ms);

    std::println("Shutdown complete");
}

int main()
{
    aio::IoContext ctx;

    auto task = AsyncMain(ctx);
    ctx.RunUntilDone(task);

    return 0;
}