// curl -v http://127.0.0.1:8080/
// curl -v --http1.1 -H 'Connection: close' http://127.0.0.1:8080/
//  printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | nc 127.0.0.1 8080

#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

#include "uring/core/task.hpp"
#include "uring/extention/io_pool.hpp"
#include "uring/logger.hpp"
#include "uring/tcp_listener.hpp"

using namespace URing;

namespace
{
constexpr bool kUseRemoteDispatch = false;
volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int)
{
    g_stop_requested = 1;
}
}  // namespace

// A static, valid HTTP/1.1 response with keep-alive
constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, io_uring!";

// Handle a single client connection
static Task<void> handle_client(IO& worker, Fd client_fd)
{
    std::byte buf[1024];

    while (true)
    {
        auto read_res = co_await worker.read(client_fd, std::span{buf});
        if (!read_res)
        {
            co_return std::unexpected(read_res.error());
        }

        if (*read_res == 0)
        {
            break;
        }

        std::span out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()), kHttpResponse.size());

        auto write_res = co_await worker.write(client_fd, out_buf);
        if (!write_res)
        {
            co_return std::unexpected(write_res.error());
        }

        if (*write_res == 0)
        {
            break;
        }
    }
    co_return {};
}

// Accept incoming connections on a single worker
static Task<void> server_loop(IO& worker, uint16_t port, std::stop_token st)
{
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "[Worker " << worker.id() << "] Failed to bind: " << listener.error().message() << "\n";
        co_return std::unexpected(listener.error());
    }

    std::cout << "[Worker " << worker.id() << "] Listening on http://0.0.0.0:" << port << "\n";

    Fd server_fd = std::move(*listener);

    while (!st.stop_requested())
    {
        auto client_res = co_await worker.accept(server_fd);
        if (!client_res)
        {
            co_return std::unexpected(client_res.error());
        }

        // Schedule the client handler on the same worker
        worker.schedule(handle_client(worker, std::move(*client_res)));
    }
    co_return {};
}

// Accept connections on one worker and dispatch to others
static Task<void> dispatching_server_loop(IoContext& ctx, IO& dispatcher, uint16_t port, std::stop_token st)
{
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "[Dispatcher] Failed to bind: " << listener.error().message() << "\n";
        co_return std::unexpected(listener.error());
    }

    std::cout << "[Dispatcher] Listening on http://0.0.0.0:" << port << " and dispatching to " << ctx.worker_count() - 1
              << " workers.\n";

    Fd server_fd = std::move(*listener);
    std::size_t next_worker = 1;

    while (!st.stop_requested())
    {
        auto client_res = co_await dispatcher.accept(server_fd);
        if (!client_res)
        {
            co_return std::unexpected(client_res.error());
        }

        const std::size_t worker_count = ctx.worker_count();
        // Dispatch to workers 1..N (round-robin)
        const std::size_t target_idx = worker_count > 1 ? 1 + ((next_worker++ - 1) % (worker_count - 1)) : 0;

        IO& target = ctx.worker(target_idx);

        // Option 1: Direct scheduling on target worker (Thread-safe)
        target.schedule(handle_client(target, std::move(*client_res)));

        /*
        // Option 2: Using TransferTo (Demonstration)
        usage: co_await io.schedule_on(other);
        */
    }
    co_return {};
}

int main()
{
    URing::ALOG::set_level(ALOG::Level::Debug);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    size_t num_threads = 4;
    if constexpr (kUseRemoteDispatch)
    {
        num_threads += 1;  // 1 dispatcher + 4 workers
    }

    // New decentralized architecture: workers start themselves in the constructor
    IoOptions opts;
    IoContext ctx(num_threads, opts);
    constexpr uint16_t port = 8080;

    auto st = ctx.stop_token();

    if constexpr (kUseRemoteDispatch)
    {
        // Start dispatcher on worker 0
        ctx.worker(0).schedule(dispatching_server_loop(ctx, ctx.worker(0), port, st));
    }
    else
    {
        // Parallel accept on all workers (SO_REUSEPORT)
        for (std::size_t i = 0; i < num_threads; ++i)
        {
            ctx.worker(i).schedule(server_loop(ctx.worker(i), port, st));
        }
    }

    std::cout << "HTTP Server running. Press Ctrl+C to stop.\n";

    while (g_stop_requested == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ctx.join();

    std::cout << "Server shutdown complete. Goodbye!\n";
    return 0;
}
