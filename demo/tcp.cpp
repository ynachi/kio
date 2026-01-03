//
// Created by Yao ACHI on 04/10/2025.
//
#include "kio/core/async_logger.h"
#include "kio/core/worker_pool.h"
#include "kio/net/net.h"

#include <csignal>
#include <format>
#include <iostream>

using namespace kio::io;
using namespace kio;

DetachedTask HandleClientHttp(Worker& worker, const int client_fd)
{
    char buffer[8192];

    // Critical! We want any client code to be stopped when the worker exits.
    // So the loop below will be synchronized on the worker's top token.
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

// User defines their application logic as coroutines
DetachedTask handle_client(Worker& worker, const int client_fd)
{
    char buffer[8192];
    const auto st = worker.GetStopToken();

    while (!st.stop_requested())
    {
        // Read from the client - this co_await runs on the worker thread
        auto n = co_await worker.AsyncRead(client_fd, std::span(buffer, sizeof(buffer)));
        if (!n.has_value())
        {
            ALOG_DEBUG("Read failed: {}", n.error());
            // in the io_uring world, most of the errors are fatal, so no need to specialize
            break;
        }

        if (n == 0)
        {
            ALOG_INFO("Client disconnected");
            break;
        }

        const size_t read_size = n.value();

        // Write response - this co_await also runs on the worker thread

        if (auto sent = co_await worker.AsyncWrite(client_fd, std::span(buffer, read_size)); !sent.has_value())
        {
            ALOG_ERROR("Write failed: {}", sent.error());
            break;
        }
    }

    close(client_fd);
}

// Accept loop - runs on each worker independently
DetachedTask accept_loop(Worker& worker)
{
    ALOG_INFO("Worker accepting connections");
    const auto st = worker.GetStopToken();

    // Create a listening socket
    auto server_fd_exp = net::create_tcp_server_socket("0.0.0.0", 8080, 4096);
    if (!server_fd_exp.has_value())
    {
        ALOG_ERROR("Failed to create server socket: {}", server_fd_exp.error());
        std::terminate();
    }

    auto server_fd = *server_fd_exp;

    ALOG_INFO("Listening on port 8080");

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Accept connection - blocks this coroutine until a client connects
        auto client_fd = co_await worker.AsyncAccept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (!client_fd.has_value())
        {
            ALOG_ERROR("error: {}", client_fd.error());
            continue;
        }

        ALOG_DEBUG("Accepted connection on fd {}", client_fd.value());

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        HandleClientHttp(worker, client_fd.value());
    }
    ALOG_INFO("Worker {} stop accepting connexions", worker.GetId());
}

int main()
{
    // ignore
    signal(SIGPIPE, SIG_IGN);
    // Setup logging
    alog::Configure(4096, LogLevel::kDisabled);

    // Configure workers
    WorkerConfig config{};
    config.uring_queue_depth = 16800;

    // Create a pool with 4 workers
    // Each worker will run accept_loop independently
    IOPool pool(4, config, [](Worker& worker) { accept_loop(worker); });

    ALOG_INFO("Server running with 4 workers. Press Ctrl+C to stop.");

    // Main thread waits (or handles signals)
    std::cout << "Server running. Press Enter to stop..." << std::endl;
    std::cin.get();  // Blocks until user presses Enter
    pool.Stop();

    ALOG_INFO("Server stopped from main");

    // Pool destructor stops all workers gracefully
    return 0;
}
