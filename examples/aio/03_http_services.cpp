#include "uring/context.h"

#include <csignal>
#include <iostream>
#include <string_view>

#include <sys/socket.h>

#include "uring/io.hpp"
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
DetachedTask server_loop(IoContext& ctx, uint16_t port)
{
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "Failed to bind to port " << port << ": Error " << listener.error().value() << "\n";
        co_return;
    }

    std::cout << "Listening on http://0.0.0.0:" << port << "\n";

    Fd server_fd = std::move(*listener);

    while (!global_stop_source.stop_requested())
    {
        auto client_res = co_await accept(ctx, server_fd);

        if (client_res)
        {
            handle_client(ctx, std::move(*client_res));
        }
        else
        {
            std::cerr << "Accept failed: " << client_res.error().value() << "\n";
        }
    }
}

int main()
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try
    {
        IoContext ctx{16384};

        server_loop(ctx, 8080);
        ctx.run(global_stop_source.get_token(), std::chrono::milliseconds(10));

        std::cout << "\nGraceful shutdown complete.\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
