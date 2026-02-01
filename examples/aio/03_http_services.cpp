// 03_http_service.cpp
//
// The "Kitchen Sink" Service:
// 1. Using aio::Worker to scale across all CPU cores.
// 2. Zero-Copy file serving (AsyncSendfile).
// 3. Introspection via internal stats.

#include <chrono>
#include <filesystem>
#include <format>
#include <print>
#include <thread>
#include <vector>

#include <fcntl.h>

#include <sys/stat.h>

#include "aio/aio.hpp"
#include "aio/core/io_helpers.hpp"
#include <gflags/gflags.h>

DEFINE_string(host, "127.0.0.1", "Server host");
DEFINE_uint32(port, 8080, "Server port");
DEFINE_string(sering_folder, "/home/ynachi/benchmarks", "The folder from which static files are served");
DEFINE_uint32(cores, 4, "number of cores");

using namespace std::chrono_literals;

namespace
{
// A basic HTTP handler
aio::Task<> HandleHttp(aio::IoContext& ctx, aio::net::Socket sock, const std::stop_token st)
{
    std::array<std::byte, 1024> buf{};

    while (!st.stop_requested())
    {
        // Read Request
        auto recv_res = co_await aio::AsyncRecv(ctx, sock, buf).WithTimeout(10s);

        if (!recv_res.has_value())
        {
            if (recv_res.error() == std::errc::timed_out)
            {
                // If we timed out, check if it's because of shutdown
                if (st.stop_requested())
                    break;

                ALOG_INFO("[Client {}] Timed out", sock.Get());
                // Optional: Send close frame
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

        std::string_view request(reinterpret_cast<const char*>(buf.data()), *recv_res);

        // Simple Router
        if (request.starts_with("GET / "))
        {
            // Hello World
            std::string_view body = "Hello from AIO!";
            std::string resp = std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}", body.size(), body);
            co_await aio::AsyncSend(ctx, sock, resp);
        }
        else if (request.starts_with("GET /stats "))
        {
            // 2. Metrics Endpoint
            auto stats = ctx.Stats().GetSnapshot();
            std::string body = std::format(
                "{{\n"
                "  \"ops_submitted\": {},\n"
                "  \"ops_completed\": {},\n"
                "  \"active_connections\": {}\n"
                "}}",
                stats.ops_submitted, stats.ops_completed, stats.ops_inflight);

            std::string resp = std::format(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}", body.size(), body);
            co_await aio::AsyncSend(ctx, sock, resp);
        }
        else if (request.starts_with("GET /file "))
        {
            // 3. Zero-Copy File Send
            const std::string filepath = std::format("{}/10g.bin", FLAGS_sering_folder);
            auto file_res = co_await aio::AsyncOpen(ctx, filepath.c_str(), O_RDONLY);
            if (file_res)
            {
                int file_fd = *file_res;
                struct stat stt{};
                fstat(file_fd, &stt);

                std::string header = std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n", stt.st_size);
                co_await aio::AsyncSend(ctx, sock, header);
                co_await aio::AsyncSendfile(ctx, sock.Get(), aio::FDGuard(file_fd), 0, stt.st_size);
            }
            else
            {
                co_await aio::AsyncSend(ctx, sock, "HTTP/1.1 404 Not Found\r\n\r\n");
            }
        }
        else
        {
            co_await aio::AsyncSend(ctx, sock, "HTTP/1.1 404 Not Found\r\n\r\n");
        }
    }
}

aio::Task<> Server(aio::IoContext& ctx, const std::string& host, uint16_t port, std::stop_token st)
{
    auto bind_res = aio::net::TcpListener::BindV4(port, host);

    if (!bind_res.has_value())
    {
        ALOG_ERROR("failed to bind {}:{} error{}", host, port, bind_res.error().message());
    }

    aio::TaskGroup tasks;
    const auto listener = std::move(bind_res.value());

    // Loop until stop requested
    while (!st.stop_requested())
    {
        // We MUST use a timeout here. If we don't, AsyncAccept will block forever,
        // preventing the loop from checking st.stop_requested().
        auto accept_res = co_await aio::AsyncAccept(ctx, listener).WithTimeout(1s);

        if (!accept_res.has_value())
        {
            if (accept_res.error() != std::errc::timed_out)
            {
                ALOG_ERROR("Accept failed: {}", accept_res.error().message());
                co_await aio::AsyncSleep(ctx, 100ms);
            }
            // If timed out, loop continues and checks st.stop_requested()
            continue;
        }

        auto socket = aio::net::Socket(accept_res.value().fd);
        ALOG_INFO("Got a client at {}:{}", *accept_res->addr.GetIp(), *accept_res->addr.GetPort());
        tasks.Spawn(HandleHttp(ctx, std::move(socket), st));
    }

    // Wait for all clients to finish (Graceful Shutdown)
    co_await tasks.JoinAll(ctx);
    ALOG_INFO("Server shutdown complete.");
}

aio::Task<> Stop(aio::IoContext& ctx, std::stop_source ss)
{
    const aio::SignalSet signals{SIGINT, SIGTERM};
    auto sig = co_await aio::AsyncWaitSignal(ctx, signals.fd());
    ALOG_WARN("\nReceived Signal {}. Shutting down...", *sig);

    // Trigger the stop token.
    // This will cause Server loops to exit and HandleHttp loops to exit.
    (void)ss.request_stop();
}
}  // namespace

int main()
{
    aio::alog::g_level = aio::alog::Level::Disabled;

    // Use std::stop_source for cooperative cancellation
    std::stop_source ss;
    std::stop_token st = ss.get_token();

    const size_t cores = FLAGS_cores;

    ALOG_INFO("Starting HTTP Service on port 8080 using {} workers...", FLAGS_cores);

    std::vector<aio::Worker> workers;
    for (size_t i = 0; i < cores; ++i)
    {
        // Do NOT pass 'st' to Worker constructor.
        // If we do, Worker auto-stops the Context when signaled, crashing pending tasks.
        // We want the Context to run until Server() returns naturally.
        workers.emplace_back(i);

        workers.back().Start(
            [st, i](aio::IoContext& ctx)
            {
                ALOG_INFO("Server {} starting on {}:{}", i, FLAGS_host, FLAGS_port);
                ctx.RunUntilDone(Server(ctx, FLAGS_host, FLAGS_port, st));
            },
            i);
    }

    // Main thread waits for signal
    aio::IoContext main_ctx;
    // Pass ss by value or ref is fine, here by value to keep it alive
    main_ctx.RunUntilDone(Stop(main_ctx, ss));

    ALOG_INFO("Shutting down workers...");
    for (auto& w : workers)
        w.Join();

    return 0;
}