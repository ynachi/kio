// curl -v http://127.0.0.1:8080/
// curl -v --http1.1 -H 'Connection: close' http://127.0.0.1:8080/
//  printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | nc 127.0.0.1 8080

#include "uring/context.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

#include "uring/io.hpp"
#include "uring/logger.hpp"
#include "uring/task.hpp"
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

// Fire-and-forget task to handle a single client connection
DetachedTask handle_client(Fd client_fd)
{
    std::byte buf[1024];

    while (true)
    {
        auto read_res = co_await read(client_fd, std::span{buf});

        if (!read_res.has_value() || *read_res == 0)
        {
            break;
        }

        std::span out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()), kHttpResponse.size());

        auto write_res = co_await write(client_fd, out_buf);

        if (!write_res.has_value() || *write_res == 0)
        {
            break;
        }
    }
}

DetachedTask handle_client_remote(Fd client_fd)
{
    std::byte buf[1024];

    while (true)
    {
        auto read_res = co_await read(client_fd, std::span{buf});

        if (!read_res.has_value() || *read_res == 0)
        {
            break;
        }

        std::span out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()), kHttpResponse.size());

        auto write_res = co_await write(client_fd, out_buf);

        if (!write_res.has_value() || *write_res == 0)
        {
            break;
        }
    }
}

// Fire-and-forget task to accept incoming connections
DetachedTask server_loop(uint16_t port, int thread_id, std::stop_token st)
{
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "[Thread " << thread_id << "] Failed to bind to port " << port << ": Error "
                  << listener.error().value() << "\n";
        co_return;
    }

    std::cout << "Listening on http://0.0.0.0:" << port << "\n";

    Fd server_fd = std::move(*listener);

    while (!st.stop_requested())
    {
        auto client_res = co_await accept(server_fd);

        if (client_res)
        {
            handle_client(std::move(client_res.value()));
        }
        else
        {
            std::cerr << "Accept failed: " << client_res.error().value() << "\n";
        }
    }
}

DetachedTask noop_worker_loop()
{
    co_return;
}

DetachedTask dispatching_server_loop(IoContext& ctx, uint16_t port, std::stop_token st)
{
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "[Dispatcher] Failed to bind to port " << port << ": Error " << listener.error().value() << "\n";
        co_return;
    }

    std::cout << "[Dispatcher] Listening on http://0.0.0.0:" << port << " and dispatching to " << ctx.worker_count()
              << " workers.\n";

    Fd server_fd = std::move(*listener);
    std::size_t next_worker = 1;

    while (!st.stop_requested())
    {
        auto client_res = co_await accept(server_fd);

        if (client_res)
        {
            const std::size_t worker_count = ctx.worker_count();
            const std::size_t target_idx = worker_count > 1 ? 1 + ((next_worker++ - 1) % (worker_count - 1)) : 0;
            IoWorker& target = ctx.worker(target_idx);
            co_await TransferTo(target);
            handle_client(std::move(client_res.value()));
        }
        else
        {
            std::cerr << "Accept failed: " << client_res.error().value() << "\n";
        }
    }
}

int main()
{
    URing::ALOG::set_level(ALOG::Level::Debug);
    ALOG_DEBUG("Debug logging enabled");

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    size_t num_threads = 4;
    if constexpr (kUseRemoteDispatch)
    {
        // add one more for benh fairness
        num_threads += 1;
    }

    constexpr IoOptions opts;
    // // lets use sqpool
    // opts.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER;
    // opts.sq_thread_idle_ms = 2000;
    // opts.sq_thread_cpu = 0;
    IoContext ctx(num_threads, opts);
    constexpr uint16_t port = 8080;

    auto st = ctx.stop_token();

    std::cout << "Starting " << num_threads << " workers...\n";

    (void)ctx.start(
        [&ctx, st, num_threads]
        {
            if constexpr (kUseRemoteDispatch)
            {
                if (IoWorker::current_io()->id() == 0)
                {
                    dispatching_server_loop(ctx, port, st);
                }
            }
            else
            {
                server_loop(port, num_threads, st);
            }
        });

    while (g_stop_requested == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ctx.stop();
    ctx.join();

    std::cout << "All workers terminated. Goodbye!\n";
    return 0;
}
