// examples/08_hello_http_v2.cpp
// HTTP "Hello World" server - Worker factory version

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "aio/io.hpp"
#include "aio/logger.hpp"
#include "aio/net.hpp"
#include "aio/task.hpp"
#include "aio/task_group.hpp"
#include "aio/worker.hpp"

using namespace std::chrono_literals;

static std::atomic g_running{true};
static std::atomic<uint64_t> g_connections{0};
static std::atomic<uint64_t> g_requests{0};

static void on_signal(int)
{
    g_running.store(false, std::memory_order_relaxed);
}

static void ignore_sigpipe()
{
    std::signal(SIGPIPE, SIG_IGN);
}

static aio::Task<> HandleClient(aio::IoContext& ctx, int fd)
{
    alignas(64) std::array<std::byte, 4096> buffer{};

    static constexpr std::string_view resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, World!";

    while (true)
    {
        auto rr = co_await aio::AsyncRecv(ctx, fd, buffer, 0);
        if (!rr || *rr == 0)
            break;

        g_requests.fetch_add(1, std::memory_order_relaxed);

        size_t sent = 0;
        while (sent < resp.size())
        {
            const auto remaining = std::string_view{resp.data() + sent, resp.size() - sent};
            auto sr = co_await aio::AsyncSend(ctx, fd, remaining, 0);
            if (!sr || *sr == 0)
                goto out;
            sent += *sr;
        }
    }

out:
    (void)co_await aio::AsyncClose(ctx, fd);
    co_return;
}

static aio::Task<> accept_loop(aio::IoContext& ctx, int listen_fd)
{
    aio::TaskGroup connections(1024);
    connections.SetSweepInterval(256);

    while (g_running.load(std::memory_order_relaxed))
    {
        auto ar = co_await aio::AsyncAccept(ctx, listen_fd);
        if (!ar)
        {
            const int e = ar.error().value();
            if (e == EBADF || e == ECANCELED)
                break;

            if (e == EMFILE || e == ENFILE)
            {
                (void)co_await aio::AsyncSleep(ctx, 10ms);
            }
            continue;
        }

        g_connections.fetch_add(1, std::memory_order_relaxed);
        connections.Spawn(HandleClient(ctx, ar.value().fd));

        if (connections.Size() > 2048)
        {
            connections.Sweep();
        }
    }

    std::fprintf(stderr, "Waiting for %zu active connections to close...\n", connections.ActiveCount());
    co_await connections.JoinAll(ctx);
    std::fprintf(stderr, "All connections closed.\n");
}

int main(int argc, char** argv)
{
    ignore_sigpipe();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const uint16_t port = (argc >= 2) ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;
    const int num_threads = (argc >= 3) ? std::max(1, std::atoi(argv[2])) : 4;

    auto socket_res = aio::net::TcpListener::Bind(port);
    if (!socket_res)
    {
        aio::alog::fatal("failed to bind to {}", port);
        return 1;
    }

    int listen_fd = socket_res->Get();
    std::fprintf(stderr, "http_hello_v2: port=%u threads=%d\n", port, num_threads);

    // Create workers
    std::vector<aio::Worker> workers;
    workers.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i)
    {
        workers.emplace_back(i);  // Worker with id = i
        workers.back().Start(
            [listen_fd](aio::IoContext& ctx)
            {
                auto acc = accept_loop(ctx, listen_fd);
                acc.Start();
                ctx.Run();
                ctx.CancelAllPending();
            },
            i);  // Pin to CPU i
    }

    // Stats reporting
    while (g_running.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(1s);
        auto c = g_connections.exchange(0, std::memory_order_relaxed);
        auto r = g_requests.exchange(0, std::memory_order_relaxed);
        if (c || r)
        {
            std::fprintf(stderr, "[stats] conns=%llu reqs=%llu\n", static_cast<unsigned long long>(c),
                         static_cast<unsigned long long>(r));
        }
    }

    std::fprintf(stderr, "shutdown...\n");
    ::close(listen_fd);

    // Clean shutdown - just call RequestStop() on each worker
    for (auto& w : workers)
    {
        w.RequestStop();
    }

    // jthread destructor auto-joins, but explicit join is clearer
    for (auto& w : workers)
    {
        w.Join();
    }

    std::fprintf(stderr, "bye.\n");
    return 0;
}
