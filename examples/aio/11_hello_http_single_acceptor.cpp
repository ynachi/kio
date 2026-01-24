// examples/09_hello_http_single_acceptor.cpp
// HTTP server with dedicated acceptor thread distributing to worker pool
// Event-driven version (no polling)

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "aio/io.hpp"
#include "aio/logger.hpp"
#include "aio/net.hpp"
#include "aio/notifier.hpp"
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

// =============================================================================
// Per-Worker Connection Queue with Notifier
// =============================================================================

class ConnectionQueue
{
public:
    void Push(int fd)
    {
        {
            std::scoped_lock lock(mutex_);
            pending_.push_back(fd);
        }
        notifier_.Signal();  // Wake the worker
    }

    std::vector<int> DrainAll()
    {
        std::scoped_lock lock(mutex_);
        std::vector<int> result;
        result.reserve(pending_.size());
        for (int fd : pending_)
        {
            result.push_back(fd);
        }
        pending_.clear();
        return result;
    }

    /// Wait for new connections (call from worker thread)
    [[nodiscard]] auto Wait(aio::IoContext& ctx) { return notifier_.Wait(ctx); }

private:
    mutable std::mutex mutex_;
    std::deque<int> pending_;
    aio::Notifier notifier_;
};

// =============================================================================
// Client Handler
// =============================================================================

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

// =============================================================================
// Worker Context
// =============================================================================

struct WorkerContext
{
    aio::Worker worker;
    ConnectionQueue queue;  // Contains Notifier internally

    explicit WorkerContext(int id) : worker(id) {}

    WorkerContext(WorkerContext&&) = delete;
    WorkerContext& operator=(WorkerContext&&) = delete;
};

// =============================================================================
// Connection Handler Loop (event-driven, no polling!)
// =============================================================================

static aio::Task<> connection_handler_loop(aio::IoContext& ctx, ConnectionQueue& queue)
{
    aio::TaskGroup connections(1024);
    connections.SetSweepInterval(256);

    while (g_running.load(std::memory_order_relaxed))
    {
        // Wait for signal from acceptor (blocks efficiently in io_uring)
        auto wait_result = co_await queue.Wait(ctx).WithTimeout(100ms);

        // Timeout is fine - just loop and check g_running
        // On signal or timeout, drain any pending connections
        auto new_fds = queue.DrainAll();
        for (int fd : new_fds)
        {
            g_connections.fetch_add(1, std::memory_order_relaxed);
            connections.Spawn(HandleClient(ctx, fd));
        }

        // Periodic sweep
        if (connections.Size() > 2048)
        {
            connections.Sweep();
        }
    }

    std::fprintf(stderr, "Worker waiting for %zu connections to close...\n", connections.ActiveCount());
    co_await connections.JoinAll(ctx);
}

// =============================================================================
// Acceptor Loop
// =============================================================================

static aio::Task<> acceptor_loop(aio::IoContext& ctx, int listen_fd,
                                 const std::vector<std::unique_ptr<WorkerContext>>& workers)
{
    size_t next_worker = 0;
    const size_t num_workers = workers.size();

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

        int client_fd = ar.value().fd;

        // Round-robin distribution
        auto& target = workers[next_worker];
        target->queue.Push(client_fd);  // Push also signals the worker

        next_worker = (next_worker + 1) % num_workers;
    }

    std::fprintf(stderr, "Acceptor shutting down.\n");
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv)
{
    ignore_sigpipe();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const uint16_t port = (argc >= 2) ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;
    const int num_workers = (argc >= 3) ? std::max(1, std::atoi(argv[2])) : 4;

    auto socket_res = aio::net::TcpListener::Bind(port);
    if (!socket_res)
    {
        aio::alog::fatal("failed to bind to {}", port);
        return 1;
    }

    int listen_fd = socket_res->Get();
    std::fprintf(stderr, "http_single_acceptor: port=%u workers=%d (event-driven)\n", port, num_workers);

    // Create worker contexts
    std::vector<std::unique_ptr<WorkerContext>> workers;
    workers.reserve(num_workers);

    for (int i = 0; i < num_workers; ++i)
    {
        workers.push_back(std::make_unique<WorkerContext>(i));
    }

    // Start workers
    for (int i = 0; i < num_workers; ++i)
    {
        auto& wc = *workers[i];
        wc.worker.RunTask([&queue = wc.queue](aio::IoContext& ctx) { return connection_handler_loop(ctx, queue); },
                          i + 1);
    }

    // Start acceptor
    aio::Worker acceptor(100);
    acceptor.RunTask([listen_fd, &workers](aio::IoContext& ctx) { return acceptor_loop(ctx, listen_fd, workers); },
                     0);  // CPU 0

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

    // Shutdown
    acceptor.RequestStop();
    acceptor.Join();

    // Shutdown
    for (auto& wc : workers)
    {
        wc->worker.RequestStop();
    }
    for (auto& wc : workers)
    {
        wc->worker.Join();
    }

    std::fprintf(stderr, "bye.\n");
    return 0;
}
