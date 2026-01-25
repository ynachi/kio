//
// Created by Yao ACHI on 25/01/2026.
//
// examples/04_timeouts.cpp
// Demonstrates: WithTimeout, error handling, retry logic

#include <chrono>
#include <print>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/net.hpp"
#include "aio/task.hpp"

using namespace std::chrono_literals;

aio::Task<> ConnectWithRetry(aio::IoContext& ctx, const char* ip, uint16_t port)
{
    auto addr = aio::net::SocketAddress::V4(port, ip);

    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        std::println("Connection attempt {} to {}:{}", attempt, ip, port);

        // Create socket
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0)
        {
            std::println(stderr, "Failed to create socket");
            co_return;
        }

        // Try to connect with timeout
        auto result = co_await aio::AsyncConnect(ctx, fd, addr).WithTimeout(2s);

        if (result)
        {
            std::println("Connected successfully!");
            co_await aio::AsyncClose(ctx, fd);
            co_return;
        }

        co_await aio::AsyncClose(ctx, fd);

        if (result.error() == std::errc::timed_out)
        {
            std::println("Connection timed out");
        }
        else
        {
            std::println("Connection failed: {}", result.error().message());
        }

        // Exponential backoff
        auto delay = 500ms * (1 << (attempt - 1));
        std::println("Retrying in {}ms...", delay.count());
        co_await aio::AsyncSleep(ctx, delay);
    }

    std::println("All connection attempts failed");
}

aio::Task<> ReceiveWithTimeout(aio::IoContext& ctx, int fd)
{
    std::array<std::byte, 1024> buffer{};

    std::println("Waiting for data (5 second timeout)...");

    auto result = co_await aio::AsyncRecv(ctx, fd, buffer).WithTimeout(5s);

    if (!result)
    {
        if (result.error() == std::errc::timed_out)
        {
            std::println("No data received within timeout");
        }
        else
        {
            std::println("Receive error: {}", result.error().message());
        }
    }
    else
    {
        std::println("Received {} bytes", *result);
    }
}

int main()
{
    aio::IoContext ctx;

    // Try connecting to a non-responsive address to demonstrate timeout
    auto task = ConnectWithRetry(ctx, "10.255.255.1", 8080);
    ctx.RunUntilDone(task);

    std::println();
    std::println("Receive wwith timeout");
    // Create socket

    // auto task2 = ReceiveWithTimeout(ctx, fd);
    // ctx.RunUntilDone(task2);

    return 0;
}
