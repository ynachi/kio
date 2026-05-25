#include "../../include/uring/core/io_worker.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <vector>

#include "../../include/uring/core/task.hpp"
#include "uring/logger.hpp"
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

// Handle a single client connection
Task<void> handle_client(IO& worker, Fd client_fd)
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
            break;

        std::span<const std::byte> out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()),
                                           kHttpResponse.size());

        auto write_res = co_await worker.write(client_fd, out_buf);
        if (!write_res)
        {
            co_return std::unexpected(write_res.error());
        }

        if (*write_res == 0)
            break;
    }
    co_return {};
}

// Accept connections on one worker and dispatch to others
Task<void> dispatcher_loop(IoContext& context, IO& worker, uint16_t port)
{
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "[Dispatcher] Failed to bind: " << listener.error().message() << "\n";
        co_return std::unexpected(listener.error());
    }

    const size_t num_workers = context.worker_count();
    std::cout << "[Dispatcher] Listening on http://0.0.0.0:" << port << " and routing to " << num_workers
              << " workers.\n";

    Fd server_fd = std::move(*listener);
    size_t worker_idx = 0;

    while (!global_stop_source.stop_requested())
    {
        // Accept connection on the dispatcher's ring
        auto client_res = co_await worker.accept(server_fd);
        if (!client_res)
        {
            co_return std::unexpected(client_res.error());
        }

        const size_t selected_worker = worker_idx % num_workers;
        IO& target_worker = context.worker(selected_worker);
        worker_idx++;

        ALOG_DEBUG("Dispatching accepted client to worker {}", selected_worker);

        // Directly schedule the client handler on the target worker's thread
        target_worker.schedule(handle_client(target_worker, std::move(*client_res)));
    }
    co_return {};
}

int main()
{
    URing::ALOG::set_level(ALOG::Level::Debug);
    ALOG_DEBUG("Debug logging enabled");

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    constexpr int num_workers = 4;
    IoContext context(num_workers);

    // Schedule the dispatcher loop on the first worker
    context.worker(0).schedule(dispatcher_loop(context, context.worker(0), 8080));

    // Wait for stop signal
    while (!global_stop_source.stop_requested())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    context.join();

    std::cout << "Echo server shutdown complete.\n";
    return 0;
}
