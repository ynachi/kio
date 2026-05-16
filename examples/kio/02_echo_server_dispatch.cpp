#include "uring/context.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <vector>

#include "uring/io.hpp"
#include "uring/logger.hpp"
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

DetachedTask handle_client(Fd client_fd)
{
    std::byte buf[1024];

    while (true)
    {
        auto read_res = co_await read(client_fd, std::span{buf});

        if (!read_res.has_value() || *read_res == 0)
            break;

        std::span<const std::byte> out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()),
                                           kHttpResponse.size());

        auto write_res = co_await write(client_fd, out_buf);

        if (!write_res.has_value() || *write_res == 0)
            break;
    }
}

DetachedTask dispatcher_loop(IoContext& context, uint16_t port)
{
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "[Dispatcher] Failed to bind: " << listener.error().value() << "\n";
        co_return;
    }

    const size_t num_workers = context.worker_count();
    std::cout << "[Dispatcher] Listening on http://0.0.0.0:" << port << " and routing to " << num_workers
              << " workers.\n";

    Fd server_fd = std::move(*listener);
    size_t worker_idx = 0;

    while (!global_stop_source.stop_requested())
    {
        auto client_res = co_await accept(server_fd);

        if (client_res)
        {
            const size_t selected_worker = worker_idx % num_workers;
            IoWorker& target_worker = context.worker(selected_worker);
            worker_idx++;

            ALOG_DEBUG("Dispatching accepted client to worker {}", selected_worker);

            // Cross-thread spawn via IoContext
            context.spawn_on(target_worker, [fd = std::move(*client_res)]() mutable -> DetachedTask
                             { return handle_client(std::move(fd)); });
        }
        else
        {
            // If accept failed because of a signal or similar, we might want to continue or break
            if (client_res.error().value() != EINTR)
            {
                std::cerr << "[Dispatcher] Accept failed: " << client_res.error().value() << "\n";
            }
        }
    }
}

int main()
{
    URing::ALOG::set_level(ALOG::Level::Debug);
    ALOG_DEBUG("Debug logging enabled");

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    constexpr int num_workers = 4;
    IoContext context(num_workers);

    // We start the context. In this example, we'll run the dispatcher on the first worker
    // by spawning it right after start.
    (void)context.start(
        [&]()
        {
            if (IoWorker::current_io()->id() == 0)
            {
                dispatcher_loop(context, 8080);
            }
        });

    // Wait for stop signal
    while (!global_stop_source.stop_requested())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    context.stop();
    context.join();

    return 0;
}
