//
// tcp_fast.cpp - Benchmark version WITHOUT OpPool
// WARNING: Not safe with cancellation - for performance testing only
//
#include "kio/core/async_logger.h"
#include "kio/core/worker.h"
#include "kio/core/worker_pool.h"
#include "kio/net/net.h"

#include <csignal>
#include <format>
#include <iostream>

#include <netinet/tcp.h>

using namespace kio::io;
using namespace kio;

DetachedTask HandleClientHttp(WorkerFast& worker, const int client_fd)
{
    // TCP optimizations (matching async_simple version)
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    int bufsize = 256 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    char buffer[8192];
    const auto st = worker.GetStopToken();

    while (!st.stop_requested())
    {
        auto n = co_await worker.AsyncRead(client_fd, std::span(buffer, sizeof(buffer)));
        if (!n.has_value())
        {
            ALOG_DEBUG("Read failed {}", n.error());
            break;
        }

        if (n == 0)
        {
            ALOG_INFO("Client disconnected");
            break;
        }

        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
        auto sent = co_await worker.AsyncWrite(client_fd, std::span(response.data(), response.size()));

        if (!sent.has_value())
        {
            ALOG_DEBUG("Write failed: {}", sent.error());
            break;
        }
    }

    close(client_fd);
}

// Fast accept loop - using WorkerFast
DetachedTask accept_loop(WorkerFast& worker)
{
    ALOG_INFO("Worker accepting connections");
    const auto st = worker.GetStopToken();

    auto server_fd_exp = net::create_tcp_server_socket("0.0.0.0", 8080, 4096);
    if (!server_fd_exp.has_value())
    {
        ALOG_ERROR("Failed to create server socket: {}", server_fd_exp.error());
        std::terminate();
    }

    auto server_fd = *server_fd_exp;

    // TCP optimizations (matching async_simple version)
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    int bufsize = 2 * 1024 * 1024;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    int qlen = 1024;
    setsockopt(server_fd, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));

    ALOG_INFO("Listening on port 8080");

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        auto client_fd = co_await worker.AsyncAccept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (!client_fd.has_value())
        {
            ALOG_ERROR("error: {}", client_fd.error());
            continue;
        }

        // Spawn detached task
        HandleClientHttp(worker, client_fd.value());
    }
    ALOG_INFO("Worker {} stop accepting connections", worker.GetId());
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    alog::Configure(4096, LogLevel::kDisabled);

    WorkerConfig config{};
    config.uring_queue_depth = 32768;  // Match async_simple

    IOPoolFast pool(4, config, [](WorkerFast& worker) { accept_loop(worker); });

    ALOG_INFO("FAST Server running with 4 workers (NO OpPool). Press Ctrl+C to stop.");
    std::cout << "FAST Server running (NO OpPool). Press Enter to stop..." << std::endl;

    std::cin.get();
    pool.Stop();

    ALOG_INFO("Server stopped");

    return 0;
}