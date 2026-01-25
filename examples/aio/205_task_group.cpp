//
// Created by Yao ACHI on 25/01/2026.
//
// examples/05_task_group.cpp
// Demonstrates: TaskGroup, concurrent execution, JoinAll

#include <chrono>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/task.hpp"
#include "aio/task_group.hpp"

using namespace std::chrono_literals;

// Simulated async work (e.g., downloading a file)
aio::Task<> FetchUrl(aio::IoContext& ctx, std::string url, int id)
{
    std::println("[{}] Starting fetch: {}", id, url);

    // Simulate variable network latency
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(100, 500);
    auto delay = std::chrono::milliseconds(dist(rng));

    co_await aio::AsyncSleep(ctx, delay);

    std::println("[{}] Completed fetch: {} ({}ms)", id, url, delay.count());
}

aio::Task<> AsyncMain(aio::IoContext& ctx)
{
    std::vector<std::string> urls = {
        "https://example.com/file1", "https://example.com/file2", "https://example.com/file3",
        "https://example.com/file4", "https://example.com/file5",
    };

    std::println("Fetching {} URLs concurrently...\n", urls.size());

    auto start = std::chrono::steady_clock::now();

    aio::TaskGroup group(10);

    int id = 0;
    for (const auto& url : urls)
    {
        group.Spawn(FetchUrl(ctx, url, ++id));
    }

    std::println("All tasks spawned, waiting for completion...\n");

    co_await group.JoinAll(ctx);

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

    std::println("\nAll {} fetches completed in {}ms", urls.size(), ms.count());
    std::println("(Sequential would have taken ~1500ms)");
}

int main()
{
    aio::IoContext ctx;

    auto task = AsyncMain(ctx);
    ctx.RunUntilDone(task);

    return 0;
}