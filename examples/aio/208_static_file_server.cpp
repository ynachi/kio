// examples/08_static_file_server.cpp
// Demonstrates: AsyncSendfile, AsyncOpen, efficient file serving

#include <array>
#include <print>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/io_helpers.hpp"
#include "aio/net.hpp"
#include "aio/task.hpp"
#include "aio/task_group.hpp"

aio::Task<> serve_file(aio::IoContext& ctx, int client_fd, const char* filepath)
{
    // Open the file
    auto file_fd = co_await aio::AsyncOpen(ctx, filepath, O_RDONLY, 0);
    if (!file_fd)
    {
        static constexpr std::string_view not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 9\r\n"
            "\r\n"
            "Not Found";
        co_await aio::AsyncSend(ctx, client_fd, not_found);
        co_await aio::AsyncClose(ctx, client_fd);
        co_return;
    }

    // Get file size
    struct stat st{};
    if (::fstat(*file_fd, &st) < 0)
    {
        co_await aio::AsyncClose(ctx, *file_fd);
        co_await aio::AsyncClose(ctx, client_fd);
        co_return;
    }

    // Send HTTP header
    std::array<char, 256> header{};
    int header_len = std::snprintf(header.data(), header.size(),
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %ld\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n", st.st_size);

    co_await aio::AsyncSend(ctx, client_fd,
        std::string_view{header.data(), static_cast<size_t>(header_len)});

    // Zero-copy file transfer!
    auto result = co_await aio::AsyncSendfile(ctx, client_fd, *file_fd, 0, st.st_size);
    if (result)
    {
        std::println("Served {} ({} bytes)", filepath, st.st_size);
    }
    else
    {
        std::println(stderr, "Sendfile failed: {}", result.error().message());
    }

    co_await aio::AsyncClose(ctx, *file_fd);
    co_await aio::AsyncClose(ctx, client_fd);
}

aio::Task<> handle_client(aio::IoContext& ctx, int fd, const char* serve_path)
{
    std::array<std::byte, 1024> buffer{};

    // Read HTTP request (simplified - just wait for any data)
    auto recv_result = co_await aio::AsyncRecv(ctx, fd, buffer);
    if (!recv_result || *recv_result == 0)
    {
        co_await aio::AsyncClose(ctx, fd);
        co_return;
    }

    // Serve the configured file for any request
    co_await serve_file(ctx, fd, serve_path);
}

aio::Task<> server(aio::IoContext& ctx, uint16_t port, const char* filepath)
{
    auto listener = aio::net::TcpListener::Bind(port);
    if (!listener)
    {
        std::println(stderr, "Failed to bind to port {}", port);
        co_return;
    }

    std::println("Serving {} on http://localhost:{}/", filepath, port);

    aio::TaskGroup clients;

    while (true)
    {
        auto accept_result = co_await aio::AsyncAccept(ctx, listener->Get());
        if (!accept_result)
            continue;

        clients.Spawn(handle_client(ctx, accept_result->fd, filepath));

        if (clients.Size() > 100)
            clients.Sweep();
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::println(stderr, "Usage: {} <port> <filepath>", argv[0]);
        return 1;
    }

    uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
    const char* filepath = argv[2];

    aio::IoContext ctx;

    auto task = server(ctx, port, filepath);
    ctx.RunUntilDone(task);

    return 0;
}