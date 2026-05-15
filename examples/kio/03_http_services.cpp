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

int main()
{
    URing::ALOG::set_level(ALOG::Level::Info);
    ALOG_DEBUG("Debug logging enabled");

    IoContext ctx(4);

    auto st = ctx.stop_token();

    // std::signal(SIGINT, ctx.stop());
    // std::signal(SIGTERM, signal_handler);

    constexpr uint16_t port = 8080;
    constexpr int num_threads = 4;

    std::cout << "Starting " << num_threads << " workers...\n";

    auto app = [port, num_threads, st](IoWorker& io) -> DetachedTask { server_loop(port, num_threads, st); };

    (void)ctx.start(app);

    std::cout << "All workers terminated. Goodbye!\n";
    return 0;
}
