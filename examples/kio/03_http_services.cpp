// curl -v http://127.0.0.1:8080/
// curl -v --http1.1 -H 'Connection: close' http://127.0.0.1:8080/
//  printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | nc 127.0.0.1 8080

#include "uring/context.h"

#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

#include "uring/io.hpp"
#include "uring/logger.hpp"
#include "uring/task.hpp"
#include "uring/tcp_listener.hpp"

using namespace URing;

// A static, valid HTTP/1.1 response with keep-alive
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

// Fire-and-forget task to handle a single client connection
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

        std::span out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()), kHttpResponse.size());

        auto write_res = co_await write(ctx, client_fd, out_buf);

        if (!write_res.has_value() || *write_res == 0)
        {
            break;
        }
    }
}

// Fire-and-forget task to accept incoming connections
DetachedTask server_loop(IoContext& ctx, uint16_t port, int thread_id)
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

    while (!global_stop_source.stop_requested())
    {
        auto client_res = co_await accept(ctx, server_fd);

        if (client_res)
        {
            if (!ctx.spawn([client_fd = std::move(*client_res)](IoContext& spawn_ctx) mutable -> DetachedTask
                           { return handle_client(spawn_ctx, std::move(client_fd)); }))
            {
                std::cerr << "Failed to spawn client handler\n";
            }
        }
        else
        {
            std::cerr << "Accept failed: " << client_res.error().value() << "\n";
        }
    }
}

// The worker function executed by each thread
void worker_thread(uint16_t port, int thread_id)
{
    try
    {
        // IoContext::pin_to_cpu(thread_id);
        IoContext ctx{16384};
        server_loop(ctx, port, thread_id);

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
    URing::ALOG::set_level(ALOG::Level::Debug);
    ALOG_DEBUG("Debug logging enabled");
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    constexpr uint16_t port = 8080;
    constexpr int num_threads = 8;

    std::cout << "Starting " << num_threads << " workers...\n";

    std::vector<std::jthread> threads;
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(worker_thread, port, i);
    }

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
