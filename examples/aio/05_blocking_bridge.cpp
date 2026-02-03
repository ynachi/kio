// 05_blocking_bridge.cpp
//
// The Bridge:
// 1. Using a thread pool (BlockingPool) for CPU-heavy or blocking tasks.
// 2. Offloading DNS resolution.
// 3. Offloading complex calculation.

#include "kio/aio.hpp"
#include "kio/core/blocking_pool.hpp"

#include <print>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// A CPU-heavy function (simulating password hashing or image processing)
uint64_t HeavyCalculation(int seed)
{
    // Simulate work
    std::this_thread::sleep_for(200ms);
    return static_cast<uint64_t>(seed) * 123456789;
}

aio::Task<> AsyncMain(aio::IoContext& ctx)
{
    // 1. Create a pool of worker threads
    aio::BlockingPool pool(4);

    std::println("1. Async DNS Resolution...");

    // kio::net::ResolveAsync internally uses Offload to call getaddrinfo
    auto addr = co_await aio::net::ResolveAsync(ctx, pool, "google.com", 80);

    if (addr)
        std::println("   Result: {}", addr->GetIp().value_or("?"));
    else
        std::println("   DNS Failed: {}", addr.error().message());

    std::println("\n2. Offloading CPU Tasks...");

    // Submit 3 tasks in parallel
    // Note: This coroutine suspends here, but the pool runs them in parallel.
    // To run parallel *from the perspective of this task*, we'd spawn sub-tasks.
    // Here we demo simple sequential offload.
    for (int i = 0; i < 3; ++i)
    {
        // "Offload" moves execution to the pool, then resumes here when done.
        auto result = co_await aio::Offload(ctx, pool, [i] { return HeavyCalculation(i); });

        std::println("   Task {} result: {}", i, result);
    }

    std::println("\nDone.");
}

int main()
{
    aio::IoContext ctx;
    ctx.RunUntilDone(AsyncMain(ctx));
    return 0;
}