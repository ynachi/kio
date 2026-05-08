#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "uring/context.h"
#include "uring/io.hpp"
#include "uring/task.hpp"
#include "uring/tcp_listener.hpp"

using namespace URing;

constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, io_uring!";

std::stop_source global_stop_source;

void signal_handler(int)
{
    global_stop_source.request_stop();
}

DetachedTask handle_client(IoContext& ctx, Fd client_fd)
{
    std::byte buf[1024];

    while (true)
    {
        auto read_res = co_await read(ctx, client_fd, std::span{buf});

        if (!read_res.has_value() || *read_res == 0)
        {
            break;
        }

        std::span<const std::byte> out_buf(
            reinterpret_cast<const std::byte*>(kHttpResponse.data()),
            kHttpResponse.size()
        );

        auto write_res = co_await write(ctx, client_fd, out_buf);

        if (!write_res.has_value() || *write_res == 0)
        {
            break;
        }
    }
}

DetachedTask server_loop(IoContext& ctx, uint16_t port, int thread_id)
{
    // The underlying Bind() uses SO_REUSEPORT, allowing multiple threads
    // to bind to the same port. The kernel will round-robin connections.
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "[Thread " << thread_id << "] Failed to bind to port " << port
                  << ": Error " << listener.error().value() << "\n";
        co_return;
    }

    std::cout << "[Thread " << thread_id << "] Listening on http://0.0.0.0:" << port << "\n";

    Fd server_fd = std::move(*listener);

    while (!global_stop_source.stop_requested())
    {
        auto client_res = co_await accept(ctx, server_fd);

        if (client_res)
        {
            handle_client(ctx, std::move(*client_res));
        }
    }
}

// The worker function executed by each thread
void worker_thread(uint16_t port, int thread_id)
{
    try
    {
        // Each thread gets its own entirely isolated ring and event loop
        IoContext ctx{16384};
        server_loop(ctx, port, thread_id);

        // Block and run the event loop for this specific thread
        ctx.run(global_stop_source.get_token());

        std::cout << "[Thread " << thread_id << "] Graceful shutdown complete.\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Thread " << thread_id << "] Fatal error: " << e.what() << "\n";
    }
}

int main()
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    constexpr uint16_t port = 8080;
    constexpr int num_threads = 4;

    std::cout << "Starting " << num_threads << " workers...\n";

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(worker_thread, port, i);
    }

    // The main thread simply waits for the signal handler to trigger the stop source,
    // which cascades down to all thread event loops, allowing a clean exit.
    for (auto& t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    std::cout << "All workers terminated. Goodbye!\n";
    return 0;
}