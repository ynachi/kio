//
// Created by Yao ACHI on 04/10/2025.
//
#include <csignal>
#include <iostream>
#include <netinet/in.h>

#include "core/include/io_pool.h"

// User defines their application logic as coroutines
kio::DetachedTask handle_client(kio::IOWorker& worker, const int client_fd) {
    char buffer[8192];
    const auto st = worker.get_stop_token();

    while (!st.stop_requested()) {
        // Read from the client - this co_await runs on the worker thread
        int n = co_await worker.async_read(client_fd, std::span(buffer, sizeof(buffer)), -1);
        if (n < 0)
        {
            spdlog::debug("Read failed {}", strerror(-n));
            // in the io_uring world, most of the errors are fatal, so no need to specialize
            break;
        }

        if (n == 0) {
            spdlog::info("Client disconnected");
            break;
        }

        // Process data (parse HTTP, handle request, etc.)
        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";

        // Write response - this co_await also runs on the worker thread
        int sent = co_await worker.async_write(
            client_fd,
            std::span(response.data(), response.size()),
            -1
        );

        if (sent < 0) {
            spdlog::error("Write failed: {}", strerror(-sent));
            break;
        }
    }

    close(client_fd);
}

// Accept loop - runs on each worker independently
kio::DetachedTask accept_loop(kio::IOWorker& worker, int listen_fd) {
    spdlog::info("Worker accepting connections");
    const auto st = worker.get_stop_token();

    while (!st.stop_requested()) {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Accept connection - blocks this coroutine until client connects
        int client_fd = co_await worker.async_accept(
            listen_fd,
            reinterpret_cast<sockaddr*>(&client_addr),
            &addr_len
        );

        if (client_fd < 0) {
            spdlog::error("Accept failed: {}", strerror(-client_fd));
            continue;
        }

        spdlog::debug("Accepted connection on fd {}", client_fd);

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        handle_client(worker, client_fd).detach();
    }
    spdlog::info("Worker {} stop accepting connexions", worker.get_id());
}

// Main function - user's entry point
using namespace kio;
int main() {
    // ignore
    signal(SIGPIPE, SIG_IGN);
    // Setup logging
    spdlog::set_level(spdlog::level::info);

    // Create a listening socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        // spdlog::error("failed to bind socket: {}", strerror(-errno));
        throw std::system_error(errno, std::system_category(), "bind failed");
    }
    // spdlog::debug("bound socket to {}:{}", ip_address, port);

    if (listen(server_fd, 1024) < 0)
    {
        // spdlog::error("failed to listen on socket: {}", strerror(-errno));
        throw std::system_error(errno, std::system_category(), "listen failed");
    }

    spdlog::info("Listening on port 8080");

    // Configure workers
    IoWorkerConfig config{};
    config.uring_queue_depth = 2048;
    config.default_op_slots = 4096;
    socklen_t addrlen = sizeof(addr);

    // Create pool with 4 workers
    // Each worker will run accept_loop independently
    IOPool pool(2, config, [server_fd](IOWorker& worker) {
        accept_loop(worker, server_fd).detach();
    });

    spdlog::info("Server running with 4 workers. Press Ctrl+C to stop.");

    // Main thread waits (or handles signals)
    // std::cout << "Server running. Press Enter to stop..." << std::endl;
    // std::cin.get();  // Blocks until user presses Enter
    // pool.stop();
    //
    // spdlog::info("Server stopped from main");
    //
    // // std::this_thread::sleep_until(std::chrono::steady_clock::time_point::max());
    //
    // // Pool destructor stops all workers gracefully
    // return 0;
}