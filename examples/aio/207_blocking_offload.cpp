// examples/07_blocking_offload.cpp
// Demonstrates: BlockingPool, Offload, ResolveAsync

#include <chrono>
#include <fstream>
#include <print>
#include <string>

#include "aio/blocking_pool.hpp"
#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/net.hpp"
#include "aio/task.hpp"

using namespace std::chrono_literals;

// Simulated CPU-intensive work
uint64_t ComputeHash(const std::string& data)
{
    uint64_t hash = 0;
    for (char c : data)
    {
        hash = hash * 31 + static_cast<uint64_t>(c);
    }
    // Simulate expensive computation
    std::this_thread::sleep_for(100ms);
    return hash;
}

aio::Task<> AsyncMain(aio::IoContext& ctx, aio::BlockingPool& pool)
{
    // Example 1: Async DNS resolution
    std::println("Resolving example.com...");
    auto addr_result = co_await aio::net::ResolveAsync(ctx, pool, "example.com", 80);
    if (addr_result)
    {
        auto ip = addr_result->GetIp();
        std::println("Resolved to: {}\n", ip.value_or("unknown"));
    }
    else
    {
        std::println("DNS resolution failed\n");
    }

    // Example 2: Offload CPU-intensive work
    std::vector<std::string> data = {
        "Hello, World!",
        "The quick brown fox",
        "Async I/O is awesome",
    };

    std::println("Computing hashes on blocking pool...");

    for (const auto& s : data)
    {
        // Offload hash computation to thread pool
        auto hash = co_await aio::Offload(ctx, pool, [&s]() {
            return ComputeHash(s);
        });

        std::println("  \"{}\" -> 0x{:016x}", s, hash);
    }

    std::println("\nAll work completed!");
}

int main()
{
    aio::IoContext ctx;
    aio::BlockingPool pool(4);  // 4 worker threads

    auto task = AsyncMain(ctx, pool);
    ctx.RunUntilDone(task);

    return 0;
}