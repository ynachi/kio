//
// Created by Yao ACHI on 04/10/2025.
//
#include <csignal>
#include <format>
#include <iostream>
#include <netinet/in.h>

#include "../kio/core/async_logger.h"
#include "../kio/core/worker_pool.h"
#include "kio/include/net.h"

using namespace kio::io;
using namespace kio;

// User defines their application logic as coroutines
DetachedTask handle_client(Worker& worker, const int client_fd)
{
    char buffer[8192];
    const auto st = worker.get_stop_token();

    while (!st.stop_requested())
    {
        // Read from the client - this co_await runs on the worker thread
        auto n = co_await worker.async_read(client_fd, std::span(buffer, sizeof(buffer)));
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

        if (auto sent = co_await worker.async_write(client_fd, std::span(buffer, read_size)); !sent.has_value())
        {
            ALOG_ERROR("Write failed: {}", sent.error());
            break;
        }
    }

    close(client_fd);
}

// Accept loop - runs on each worker independently
DetachedTask accept_loop(Worker& worker, const int listen_fd)
{
    ALOG_INFO("Worker accepting connections");
    const auto st = worker.get_stop_token();

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Accept connection - blocks this coroutine until a client connects
        auto client_fd = co_await worker.async_accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (!client_fd.has_value())
        {
            ALOG_ERROR("error: {}", client_fd.error());
            continue;
        }

        ALOG_DEBUG("Accepted connection on fd {}", client_fd.value());

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        handle_client(worker, client_fd.value()).detach();
    }
    ALOG_INFO("Worker {} stop accepting connexions", worker.get_id());
}

int main()
{
    // ignore
    signal(SIGPIPE, SIG_IGN);
    // Setup logging
    alog::configure(4096, LogLevel::Info);

    // Create a listening socket
    auto server_fd_exp = net::create_tcp_socket("0.0.0.0", 8080, 4096);
    if (!server_fd_exp.has_value())
    {
        ALOG_ERROR("Failed to create server socket: {}", server_fd_exp.error());
        return 1;
    }

    auto server_fd = *server_fd_exp;

    ALOG_INFO("Listening on port 8080");

    // Configure workers
    WorkerConfig config{};
    config.uring_queue_depth = 16800;
    config.default_op_slots = 8096;


    // Create a pool with 4 workers
    // Each worker will run accept_loop independently
    IOPool pool(4, config, [server_fd](Worker& worker) { accept_loop(worker, server_fd).detach(); });

    ALOG_INFO("Server running with 4 workers. Press Ctrl+C to stop.");

    // Main thread waits (or handles signals)
    std::cout << "Server running. Press Enter to stop..." << std::endl;
    std::cin.get();  // Blocks until user presses Enter
    pool.stop();

    ALOG_INFO("Server stopped from main");

    // Pool destructor stops all workers gracefully
    return 0;
}
