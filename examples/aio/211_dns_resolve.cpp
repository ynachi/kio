// DNS Resolution Benchmark
// Demonstrates: blocking vs async (offload) DNS, task fan-out, blocking_pool

#include "kio/aio.hpp"
#include "kio/core/blocking_pool.hpp"
#include "kio/logger.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <netdb.h>

#include <arpa/inet.h>

using namespace std::chrono_literals;

static const std::vector<std::string> domains = {
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

// Blocking DNS resolution (uses getaddrinfo)
static std::string resolve_blocking(const std::string& host) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), "80", &hints, &res) != 0 || !res) {
        return "FAILED";
    }

    char buf[INET6_ADDRSTRLEN] = {};
    if (res->ai_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, buf, sizeof(buf));
    } else if (res->ai_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(res->ai_addr)->sin6_addr, buf, sizeof(buf));
    }

    freeaddrinfo(res);
    return buf;
}

// -----------------------------------------------------------------------------
// Blocking Benchmark - Sequential, blocks the IO thread
// -----------------------------------------------------------------------------
kio::Task<> bench_blocking(kio::IoContext& ctx) {
    ALOG_INFO("--- Starting BLOCKING Benchmark (Sequential) ---");

    std::vector<std::pair<std::string, std::string>> results;
    results.reserve(domains.size());

    auto start = std::chrono::high_resolution_clock::now();

    for (const auto& domain : domains) {
        // This blocks the IO thread!
        auto ip = resolve_blocking(domain);
        results.emplace_back(domain, ip);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    ALOG_INFO("Blocking Total Time: {} ms", ms);
    ALOG_INFO("--- Results (Blocking) ---");
    for (const auto& [dom, ip] : results) {
        ALOG_INFO("{}: {}", dom, ip);
    }

    co_return;
}

// -----------------------------------------------------------------------------
// Async Benchmark - Parallel via blocking_pool
// -----------------------------------------------------------------------------

kio::Task<void> resolve_one(kio::IoContext& ctx, kio::BlockingPool& pool,
                            std::string domain,
                            std::vector<std::pair<std::string, std::string>>& results,
                            std::atomic<int>& pending) {
    // Offload blocking DNS to thread pool
    // IMPORTANT: capture domain by value - the coroutine frame may be destroyed
    auto ip = co_await kio::Offload(ctx, pool, [domain] {
        return resolve_blocking(domain);
    });

    // Back on IO thread - safe to touch results
    results.emplace_back(domain, ip);
    pending.fetch_sub(1, std::memory_order_release);
}

kio::Task<> bench_async(kio::IoContext& ctx, kio::BlockingPool& pool) {
    ALOG_INFO("--- Starting ASYNC Benchmark (Parallel) ---");

    std::vector<std::pair<std::string, std::string>> results;
    results.reserve(domains.size());

    auto start = std::chrono::high_resolution_clock::now();

    std::atomic<int> pending{static_cast<int>(domains.size())};

    // Keep tasks alive
    std::vector<kio::Task<>> tasks;
    tasks.reserve(domains.size());

    // Fan-out: spawn all tasks
    for (const auto& domain : domains) {
        auto t = resolve_one(ctx, pool, domain, results, pending);
        t.Start();
        tasks.push_back(std::move(t));
    }

    // Wait for completion
    while (pending.load(std::memory_order_acquire) > 0) {
        co_await kio::AsyncSleep(ctx, 1ms);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    kio::alog::info("Async Total Time: {} ms", ms);
    kio::alog::info("--- Results (Async) ---");
    for (const auto& [dom, ip] : results) {
        kio::alog::info("{}: {}", dom, ip);
    }

    co_return;
}

int main() {
    try {
        kio::alog::start();

        kio::IoContext ctx;
        kio::BlockingPool pool{20};  // 20 threads for parallel DNS

        // Run blocking benchmark
        auto t1 = bench_blocking(ctx);
        ctx.RunUntilDone(t1);

       // std::cout << std::endl;

        // Run async benchmark
        auto t2 = bench_async(ctx, pool);
        ctx.RunUntilDone(std::move(t2));

        kio::alog::stop();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}