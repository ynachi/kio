#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "kio/kio_logger.hpp"
#include "kio/net.hpp"
#include "kio/io.hpp"
#include "kio/runtime.hpp"
#include <arpa/inet.h>

using namespace kio;
using namespace kio::net;

const std::vector<std::string> domains = {
    // Original 10
    "google.com",
    "github.com",
    "stackoverflow.com",
    "reddit.com",
    "amazon.com",
    "microsoft.com",
    "wikipedia.org",
    "youtube.com",
    "x.com",
    "instagram.com",
    // New 10
    "netflix.com",
    "twitch.tv",
    "linkedin.com",
    "apple.com",
    "adobe.com",
    "openai.com",
    "zoom.us",
    "spotify.com",
    "ebay.com",
    "pinterest.com"
};

// Helper to convert SocketAddress to string IP
std::string ip_to_string(const SocketAddress& addr)
{
    char buf[INET6_ADDRSTRLEN] = {0};
    const sockaddr* sa = addr.get();
    if (sa->sa_family == AF_INET)
    {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, buf, sizeof(buf));
    }
    else if (sa->sa_family == AF_INET6)
    {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

// -----------------------------------------------------------------------------
// Blocking Benchmark
// Calls getaddrinfo directly on the IO thread. Blocks everything.
// -----------------------------------------------------------------------------
Task<> bench_blocking(ThreadContext& ctx)
{
    log::info("--- Starting BLOCKING Benchmark (Sequential) ---");
    std::vector<std::pair<std::string, std::string>> results;
    results.reserve(domains.size());

    auto start = std::chrono::high_resolution_clock::now();

    for (const auto& domain : domains)
    {
        // This blocks the IO thread!
        auto res = SocketAddress::resolve(domain, 80);
        if (res)
        {
            results.emplace_back(domain, ip_to_string(*res));
        }
        else
        {
            log::error("Failed {}", domain);
            results.emplace_back(domain, "FAILED");
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    log::info("Blocking Total Time: {} ms", ms);

    log::info("--- Results (Blocking) ---");
    for (const auto& [dom, ip] : results)
    {
        log::info("{}: {}", dom, ip);
    }
    co_return;
}

// -----------------------------------------------------------------------------
// Async Benchmark
// Spawns tasks to the thread pool. Runs in parallel.
// -----------------------------------------------------------------------------

// Helper to wrap the resolution in a fire-and-forget task
Task<> resolve_wrapper(ThreadContext& ctx, std::string domain, std::atomic<int>& counter, std::vector<std::pair<std::string, std::string>>& results)
{
    // This suspends and runs on the blocking thread pool
    auto res = co_await SocketAddress::resolve_async(ctx, domain, 80);

    if (res)
    {
        // We are back on the IO thread here (ctx), so pushing to vector is thread-safe
        // relative to other wrapper completions running on the same ctx.
        results.emplace_back(domain, ip_to_string(*res));
    }
    else
    {
        log::error("Failed {}", domain);
        results.emplace_back(domain, "FAILED");
    }

    counter.fetch_sub(1, std::memory_order_release);
}

Task<> bench_async(ThreadContext& ctx)
{
    log::info("--- Starting ASYNC Benchmark (Parallel) ---");
    std::vector<std::pair<std::string, std::string>> results;
    results.reserve(domains.size());

    auto start = std::chrono::high_resolution_clock::now();

    std::atomic<int> pending = domains.size();

    // Fan-out: Spawn all tasks immediately using schedule()
    for (const auto& domain : domains)
    {
        // schedule() enqueues the task to run on the event loop
        ctx.schedule(resolve_wrapper(ctx, domain, pending, results));
    }

    // Wait for completion (Simple polling for demo purposes)
    while (pending.load(std::memory_order_acquire) > 0)
    {
        co_await kio::io::timeout_ms(ctx, 1);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    log::info("Async Total Time: {} ms", ms);

    log::info("--- Results (Async) ---");
    for (const auto& [dom, ip] : results)
    {
        log::info("{}: {}", dom, ip);
    }
}

int main()
{
    try
    {
        // Setup Runtime
        RuntimeConfig config;
        config.num_threads = 1; // 1 IO thread
        config.blocking_threads = 20; // Increased workers to handle more domains

        Runtime rt(config);

        // Start the runtime threads!
        rt.loop_forever();

        ThreadContext& ctx = rt.next_thread();

        // Run Blocking Test
        block_on(ctx, bench_blocking(ctx));

        std::cout << std::endl;

        // Run Async Test
        block_on(ctx, bench_async(ctx));

        // Cleanup
        rt.stop();
    }
    catch (const std::exception& e)
    {
        log::error("Exception: {}", e.what());
        return 1;
    }

    return 0;
}