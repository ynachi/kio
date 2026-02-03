//
// Created by Yao ACHI on 01/02/2026.
//

#include "kio/aio.hpp"

#include <array>
#include <chrono>
#include <stop_token>

#include <gflags/gflags.h>

using namespace std::chrono_literals;

DEFINE_string(host, "127.0.0.1", "Server host");
DEFINE_uint32(port, 8080, "Server port");

namespace
{
kio::Task<> HandleClient(kio::IoContext& ctx, kio::net::Socket sock, const std::stop_token st)
{
    std::array<std::byte, 1024> buf{};

    while (!st.stop_requested())
    {
        // Read with a 10-second timeout.
        // If the client is idle for too long, we disconnect them.
        // Note: timeout in the hote path is expensive. It cost 2X system call per operation
        // which is huge when we have a great number of them! This is to demo the timeout capability.
        // I would not use it on this kind of operations in production. It is only fine for
        // non-frequent operations.
        auto recv_res = co_await kio::AsyncRecv(ctx, sock, buf).WithTimeout(10s);

        if (!recv_res.has_value())
        {
            if (recv_res.error() == std::errc::timed_out)
            {
                ALOG_INFO("[Client {}] Timed out", sock.Get());
                std::string_view close_msg("Timeout, the server will close the connexion");
                co_await kio::AsyncSend(ctx, sock, close_msg, close_msg.size());
            }
            else
            {
                ALOG_INFO("[Client {}] Read error: {}", sock.Get(), recv_res.error().message());
            }
            break;
        }

        if (recv_res.value() == 0)
        {
            break;
        }
        // echo back
        auto send_res = co_await kio::AsyncSend(ctx, sock, std::span{buf.data(), *recv_res});
        if (!send_res)
        {
            ALOG_INFO("[Client {}] Write error: {}", sock.Get(), send_res.error().message());
            break;
        }
    }

    ALOG_INFO("[Client {}] Client closing connexion", sock.Get());
    co_await kio::AsyncClose(ctx, sock);
}

kio::Task<> Stop(kio::IoContext& ctx, const std::stop_source ss)
{
    const kio::SignalSet signals{SIGINT, SIGTERM};
    auto sig = co_await kio::AsyncWaitSignal(ctx, signals.fd());
    ALOG_WARN("\nReceived Signal {}. Shutting down...", *sig);
    (void)ss.request_stop();
}

kio::Task<> Server(kio::IoContext& ctx, const std::string& host, uint16_t port)
{
    auto ss = std::stop_source();
    auto st = ss.get_token();

    auto bind_res = kio::net::TcpListener::BindV4(port, host);

    if (!bind_res.has_value())
    {
        ALOG_ERROR("failed to bind {}:{} error{}", host, port, bind_res.error().message());
    }

    ALOG_INFO("Server started on {}:{}", host, port);

    kio::TaskGroup tasks;
    tasks.Spawn(Stop(ctx, ss));

    const auto listener = std::move(bind_res.value());

    while (!ss.stop_requested())
    {
        // timeout sometimes to check the stop token
        auto accept_res = co_await kio::AsyncAccept(ctx, listener).WithTimeout(5s);

        if (!accept_res.has_value())
        {
            if (accept_res.error() == std::errc::timed_out)
            {
                continue;
            }
            ALOG_ERROR("Accept failed: {}", accept_res.error().message());
            // maybe transient, wait a bit
            co_await kio::AsyncSleep(ctx, 100ms);
            continue;
        }

        auto socket = kio::net::Socket(accept_res.value().fd);
        ALOG_INFO("Got a client at {}:{}", *accept_res->addr.GetIp(), *accept_res->addr.GetPort());
        tasks.Spawn(HandleClient(ctx, std::move(socket), st));
    }

    co_await tasks.JoinAll(ctx);
    ALOG_INFO("Server shutdown complete.");
}
}  // namespace

int main()
{
    kio::alog::g_level = kio::alog::Level::Info;
    kio::IoContext ctx;
    ctx.RunUntilDone(Server(ctx, FLAGS_host, FLAGS_port));
    return 0;
}