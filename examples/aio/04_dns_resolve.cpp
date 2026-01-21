// examples/dns_bench.cpp
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include "aio/aio.hpp"

#include "aio/logger.hpp"
// -----------------------------------------------------------------------------
// Domain list
// -----------------------------------------------------------------------------
static const std::vector<std::string> domains = {
    "google.com","github.com","stackoverflow.com","reddit.com","amazon.com",
    "microsoft.com","wikipedia.org","youtube.com","x.com","instagram.com",
    "netflix.com","twitch.tv","linkedin.com","apple.com","adobe.com",
    "openai.com","zoom.us","spotify.com","ebay.com","pinterest.com"
};


static std::string ip_to_string(const SocketAddress& a) {
    char buf[INET6_ADDRSTRLEN] = {0};
    const sockaddr* sa = a.get();
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        ::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
    } else if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        ::inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

} // namespace aio::net


// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
static aio::task<void> bench_blocking(aio::io_context& ctx) {
    (void)ctx; // ctx unused but signature keeps symmetry

    alog::info("--- Starting BLOCKING Benchmark (Sequential) ---");

    std::vector<std::string> ips(domains.size());
    auto t0 = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < domains.size(); ++i) {
        auto r = aio::net::SocketAddress::resolve(domains[i], 80);
        if (r) ips[i] = aio::net::ip_to_string(*r);
        else   ips[i] = "FAILED";
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    alog::info("Blocking Total Time: {} ms", ms);
    alog::info("--- Results (Blocking) ---");
    for (size_t i = 0; i < domains.size(); ++i) {
        alog::info("{}: {}", domains[i], ips[i]);
    }

    co_return;
}

static aio::task<void> resolve_one(aio::io_context& ctx,
                                  aio::blocking_pool& pool,
                                  std::string domain,
                                  size_t index,
                                  std::vector<std::string>& ips,
                                  std::atomic<int>& pending) {
    try {
        // Offload getaddrinfo to pool; resume on ctx via MSG_RING completion.
        auto r = co_await aio::offload(ctx, pool, [d = std::move(domain)]() {
            return aio::net::SocketAddress::resolve(d, 80);
        });

        if (r) ips[index] = aio::net::ip_to_string(*r);
        else   ips[index] = "FAILED";
    } catch (...) {
        ips[index] = "FAILED";
    }

    pending.fetch_sub(1, std::memory_order_release);
    co_return;
}

static aio::task<void> bench_async(aio::io_context& ctx, aio::blocking_pool& pool) {
    alog::info("--- Starting ASYNC Benchmark (Parallel) ---");

    std::vector<std::string> ips(domains.size());
    std::vector<aio::task<void>> tasks;
    tasks.reserve(domains.size());

    std::atomic<int> pending{static_cast<int>(domains.size())};

    auto t0 = std::chrono::high_resolution_clock::now();

    // Fan-out: start all tasks quickly; they immediately offload + suspend.
    for (size_t i = 0; i < domains.size(); ++i) {
        auto t = resolve_one(ctx, pool, domains[i], i, ips, pending);
        t.start();
        tasks.push_back(std::move(t));
    }

    // Wait for completion (cheap polling using io_uring timer)
    while (pending.load(std::memory_order_acquire) > 0) {
        (void)co_await aio::async_sleep(&ctx, std::chrono::milliseconds(1));
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    alog::info("Async Total Time: {} ms", ms);
    alog::info("--- Results (Async) ---");
    for (size_t i = 0; i < domains.size(); ++i) {
        alog::info("{}: {}", domains[i], ips[i]);
    }

    co_return;
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main() {
    #if __has_include("aio/logger.hpp")
    aio::alog::start();
    aio::alog::g_level.store(aio::alog::Level::Info, std::memory_order_relaxed);
    #endif

    try {
        aio::io_context ctx(1024);
        aio::blocking_pool pool(/*threads=*/20);

        {
            auto t = bench_blocking(ctx);
            ctx.run_until_done(t);
        }

        std::fprintf(stderr, "\n");

        {
            auto t = bench_async(ctx, pool);
            ctx.run_until_done(t);
        }

        // Ensure nothing pending before destruction (demo hygiene)
        ctx.cancel_all_pending();

        aio::alog::stop();

        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        aio::alog::stop();
        return 1;
    }
}