// examples/08_hello_http_v2.cpp
// HTTP "Hello World" server - Modernized version
//
// Changes from original:
// 1. std::span for buffer safety
// 2. io_context& instead of io_context*
// 3. task_group for automatic lifetime management
// 4. std::array instead of C-style arrays
// 5. std::string_view for zero-copy responses

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <sys/socket.h>

#include "aio/events.hpp"
#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/logger.hpp"
#include "aio/net.hpp"
#include "aio/task.hpp"
#include "aio/task_group.hpp"
#include <netinet/in.h>
#include <netinet/tcp.h>

using namespace std::chrono_literals;

static std::atomic     g_running{true};
static std::atomic<uint64_t> g_connections{0};
static std::atomic<uint64_t> g_requests{0};

static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }
static void ignore_sigpipe() { std::signal(SIGPIPE, SIG_IGN); }

static void pin_to_cpu(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id % static_cast<int>(std::thread::hardware_concurrency()), &cpuset);

    if (int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0) {
        std::fprintf(stderr, "Warning: Failed to pin thread to CPU %d: %s\n", cpu_id, strerror(rc));
    } else {
        std::fprintf(stderr, "Thread pinned to CPU %d\n", cpu_id);
    }
}

static aio::Task<> handle_client(aio::IoContext& ctx, int fd) {

    alignas(64) std::array<std::byte, 4096> buffer{};

    // CHANGE 2: std::string_view for zero-copy response
    static constexpr std::string_view resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, World!";

    while (true) {
        auto rr = co_await aio::AsyncRecv(ctx, fd, buffer, 0);
        if (!rr || *rr == 0) break;

        g_requests.fetch_add(1, std::memory_order_relaxed);

        size_t sent = 0;
        while (sent < resp.size()) {
            // Subspan for partial sending
            const auto remaining = std::string_view{resp.data() + sent, resp.size() - sent};
            auto sr = co_await aio::AsyncSend(ctx, fd, remaining, 0);
            if (!sr || *sr == 0) goto out;
            sent += *sr;
        }
    }

out:
    (void)co_await aio::AsyncClose(ctx, fd);
    co_return;
}

// =============================================================================
// Modernized Accept Loop
// =============================================================================

static aio::Task<> accept_loop(aio::IoContext& ctx, int listen_fd) {
    aio::TaskGroup connections(&ctx, 1024);
    connections.SetSweepInterval(256);  // Sweep every 256 connections

    while (g_running.load(std::memory_order_relaxed)) {
        auto ar = co_await aio::AsyncAccept(ctx, listen_fd);
        if (!ar) {
            const int e = ar.error().value();
            if (e == EBADF || e == ECANCELED) break;

            if (e == EMFILE || e == ENFILE) {
                (void)co_await aio::AsyncSleep(ctx, 10ms);
            }
            continue;
        }

        g_connections.fetch_add(1, std::memory_order_relaxed);

        connections.Spawn(handle_client(ctx, *ar));

        // Optional manual sweep (automatic happens every 256)
        if (connections.Size() > 2048) {
            const size_t removed = connections.Sweep();
            (void)removed;  // Could log this
        }
    }

    std::fprintf(stderr, "Waiting for %zu active connections to close...\n",
                 connections.ActiveCount());

    co_await connections.JoinAll(ctx);

    std::fprintf(stderr, "All connections closed.\n");
}

// =============================================================================
// Worker Thread
// =============================================================================

struct Worker {
    std::unique_ptr<aio::IoContext> ctx;  // Already using smart pointers!
    std::thread th;
};

int main(int argc, char** argv) {
    ignore_sigpipe();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const uint16_t port = (argc >= 2) ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;
    const int threads = (argc >= 3) ? std::max(1, std::atoi(argv[2])) : 4;

    auto socket_res = aio::net::TcpListener::Bind(port);
    if (!socket_res.has_value())
    {
        aio::alog::fatal("failed to bind to {}", port);
    }


    int listen_fd = socket_res.value().Get();
    if (listen_fd < 0) {
        std::perror("bind/listen");
        return 1;
    }

    std::fprintf(stderr, "http_hello_v2: port=%u threads=%d\n", port, threads);

    std::vector<Worker> workers;
    workers.resize(static_cast<size_t>(threads));  // Reserve space, don't construct yet

    auto uring_flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    for (int i = 0; i < threads; ++i) {
        auto& w = workers[static_cast<size_t>(i)];
        w.th = std::thread([&w, listen_fd, i, uring_flags] {
            pin_to_cpu(i);

            // Create IoContext on THIS thread
            w.ctx = std::make_unique<aio::IoContext>(16800, uring_flags);

            auto acc = accept_loop(*w.ctx, listen_fd);
            acc.Start();

            w.ctx->Run();
            w.ctx->CancelAllPending();
        });
    }

    // Stats reporting
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(1s);
        auto c = g_connections.exchange(0, std::memory_order_relaxed);
        auto r = g_requests.exchange(0, std::memory_order_relaxed);
        if (c || r) {
            std::fprintf(stderr, "[stats] conns=%llu reqs=%llu\n",
                        static_cast<unsigned long long>(c), static_cast<unsigned long long>(r));
        }
    }

    std::fprintf(stderr, "shutdown...\n");

    ::close(listen_fd);

    // Clean shutdown using ring_waker
    aio::RingWaker waker;
    for (auto& w : workers) {
        w.ctx->Stop();
        waker.Wake(*w.ctx);  // Reference deref!
    }

    for (auto& w : workers) {
        if (w.th.joinable()) w.th.join();
    }

    std::fprintf(stderr, "bye.\n");
    return 0;
}