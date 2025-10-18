//
// Created by Yao ACHI on 17/10/2025.
//

/**
 * Demonstrate the usage of an IO worker with a detached coroutine callback.
 **/

#include <iostream>

#include "core/include/io/worker.h"
#include "core/include/net.h"


using namespace kio;
using namespace kio::io;
using namespace kio::net;

// User defines their application logic as coroutines
DetachedTask HandleClient(Worker& worker, const int client_fd)
{
    char buffer[8192];
    const auto st = worker.get_stop_token();

    while (!st.stop_requested())
    {
        // Read from the client - this co_await runs on the worker thread
        int n = co_await worker.async_read(client_fd, std::span(buffer, sizeof(buffer)), -1);
        if (n < 0)
        {
            spdlog::debug("Read failed {}", strerror(-n));
            // in the io_uring world, most of the errors are fatal, so no need to specialize
            break;
        }

        if (n == 0)
        {
            spdlog::info("Client disconnected");
            break;
        }

        // Process data (parse HTTP, handle request, etc.)
        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";

        // Write response - this co_await also runs on the worker thread
        int sent = co_await worker.async_write(client_fd, std::span(response.data(), response.size()), -1);

        if (sent < 0)
        {
            spdlog::error("Write failed: {}", strerror(-sent));
            break;
        }
    }

    close(client_fd);
}


// Accept loop - runs on each worker independently
DetachedTask accept_loop(Worker& worker, int listen_fd)
{
    spdlog::info("Worker accepting connections");
    const auto st = worker.get_stop_token();

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Accept connection - blocks this coroutine until client connects
        int client_fd = co_await worker.async_accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_fd < 0)
        {
            spdlog::error("Accept failed: {}", strerror(-client_fd));
            continue;
        }

        spdlog::debug("Accepted connection on fd {}", client_fd);

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        HandleClient(worker, client_fd).detach();
    }
    spdlog::info("Worker {} stop accepting connexions", worker.get_id());
}

int main_work()
{
    // ignore signals
    signal(SIGPIPE, SIG_IGN);
    // Setup logging
    spdlog::set_level(spdlog::level::info);

    // create a server socket
    const std::string ip_address = "127.0.0.1";
    constexpr int port = 8080;

    auto server_fd = create_tcp_socket(ip_address, port, 128);
    if (!server_fd)
    {
        spdlog::error("Failed to create server socket: {}", IoErrorToString(server_fd.error()));
        return 1;
    }
    spdlog::info("server listening on endpoint: {}:{}, FD:{}", ip_address, port, server_fd.value());

    WorkerConfig config{};
    config.uring_queue_depth = 2048;
    config.default_op_slots = 4096;

    auto init_latch = std::make_shared<std::latch>(1);
    auto shutdown_latch = std::make_shared<std::latch>(1);

    Worker worker(0, config, init_latch, shutdown_latch, [server_fd](Worker& worker) { accept_loop(worker, server_fd.value()).detach(); });

    spdlog::info("Main thread: Waiting for worker to initialize...");
    init_latch->wait();
    spdlog::info("Main thread: Worker is ready.");

    // 7. The worker is now running the echo_server coroutine.
    //    The main thread can do other things or just wait.
    std::cout << "Server listening on 127.0.0.1:8080. Press Enter to stop...\n";
    std::cin.get();

    // 8. Request the worker to stop
    spdlog::info("Main thread: Requesting worker stop...");
    worker.request_stop();  // This signals the jthread and drains completions

    // 9. Wait for the worker to fully shut down
    shutdown_latch->wait();
    spdlog::info("Main thread: Worker has shut down. Exiting.");

    return 0;
}
