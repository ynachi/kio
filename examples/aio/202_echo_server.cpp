//
// Created by Yao ACHI on 24/01/2026.
//
// examples/02_echo_server.cpp
// Demonstrates: Socket, TcpListener, AsyncAccept, AsyncRecv, AsyncSend, AsyncClose

#include <array>
#include <print>

#include "aio/aio.hpp"

namespace
{
aio::Task<> HandleClient(aio::IoContext& ctx, int fd)
{
    std::array<std::byte, 1024> buffer{};

    while (true)
    {
        auto recv_result = co_await aio::AsyncRecv(ctx, fd, buffer);
        if (!recv_result || *recv_result == 0)
            break;

        auto send_result = co_await aio::AsyncSend(ctx, fd, std::span{buffer.data(), *recv_result});
        if (!send_result)
            break;
    }

    co_await aio::AsyncClose(ctx, fd);
    std::println("Client disconnected");
}

aio::Task<> Server(aio::IoContext& ctx, uint16_t port)
{
    auto listener = aio::net::TcpListener::Bind(port);
    if (!listener)
    {
        std::println(stderr, "Failed to bind to port {}", port);
        co_return;
    }

    std::println("Echo server listening on port {}", port);

    // Use TaskGroup to manage the lifetime of client tasks
    aio::TaskGroup tasks(256);

    while (true)
    {
        auto accept_result = co_await aio::AsyncAccept(ctx, listener->Get());
        if (!accept_result)
            continue;

        std::println("Client connected");

        // Start a client handler (fire and forget for simplicity)
        auto client_task = HandleClient(ctx, accept_result->fd);
        // This won't work because task need to stay alive
        // client_task.Start();

        // Spawn the task into the group.
        // TaskGroup keeps the Task object alive until it finishes.
        tasks.Spawn(HandleClient(ctx, accept_result->fd));
    }
}
}  // namespace
int main()
{
    aio::IoContext ctx;

    auto task = Server(ctx, 8080);
    ctx.RunUntilDone(task);

    return 0;
}