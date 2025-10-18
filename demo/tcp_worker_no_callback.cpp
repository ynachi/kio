//
// Created by Yao ACHI on 18/10/2025.
//
//
// Created by Yao ACHI on 17/10/2025.
//

/**
 * Demonstrate the usage of an IO worker by composing awaitable Tasks.
 * This is the ideal pattern:
 * 1. HandleClient is DetachedTask (fire-and-forget).
 * 2. accept_loop is Task<> (awaitable).
 * 3. A single "main" coroutine is launched from the callback to
 * co_await accept_loop.
 **/

#include <iostream>

#include "core/include/coro.h"
#include "core/include/io/worker.h"
#include "core/include/net.h"
#include "core/include/sync_wait.h"


using namespace kio;
using namespace kio::io;
using namespace kio::net;

//
// 1. HandleClient remains a DetachedTask.
// The accept loop should not wait for it.
//
DetachedTask HandleClient(Worker& worker, const int client_fd)
{
    char buffer[8192];
    const auto st = worker.get_stop_token();

    while (!st.stop_requested())
    {
        int n = co_await worker.async_read(client_fd, std::span(buffer, sizeof(buffer)), -1);
        if (n < 0)
        {
            spdlog::debug("Read failed {}", strerror(-n));
            break;
        }

        if (n == 0)
        {
            spdlog::info("Client disconnected");
            break;
        }

        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
        int sent = co_await worker.async_write(client_fd, std::span(response.data(), response.size()), -1);

        if (sent < 0)
        {
            spdlog::error("Write failed: {}", strerror(-sent));
            break;
        }
    }

    close(client_fd);
}


// 2. accept_loop is an awaitable Task<void>.
Task<void> accept_loop(Worker& worker, int listen_fd)
{
    spdlog::info("Worker accepting connections");
    const auto st = worker.get_stop_token();

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = co_await worker.async_accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_fd < 0)
        {
            // If stop is requested, exit
            if (st.stop_requested())
            {
                break;
            }
            spdlog::error("Accept failed: {}", strerror(-client_fd));
            continue;
        }

        spdlog::debug("Accepted connection on fd {}", client_fd);

        // This is still "fire and forget"
        HandleClient(worker, client_fd).detach();
    }

    spdlog::info("Worker {} stop accepting connexions", worker.get_id());
    co_return;
}

Task<int> main_work()
{
    signal(SIGPIPE, SIG_IGN);
    spdlog::set_level(spdlog::level::info);

    const std::string ip_address = "127.0.0.1";
    constexpr int port = 8080;

    auto server_fd = create_tcp_socket(ip_address, port, 128);
    if (!server_fd)
    {
        // Assuming IoErrorToString exists
        spdlog::error("Failed to create server socket: {}", IoErrorToString(server_fd.error()));
        co_return 1;
    }
    spdlog::info("server listening on endpoint: {}:{}, FD:{}", ip_address, port, server_fd.value());

    WorkerConfig config{};
    config.uring_queue_depth = 2048;
    config.default_op_slots = 4096;

    auto init_latch = std::make_shared<std::latch>(1);
    auto shutdown_latch = std::make_shared<std::latch>(1);

    Worker worker(0, config, init_latch, shutdown_latch);

    spdlog::info("Main thread: Waiting for worker to initialize...");
    init_latch->wait();

    co_await accept_loop(worker, server_fd.value());
    spdlog::info("Main thread: Worker is ready.");

    std::cout << "Server listening on 127.0.0.1:8080. Press Enter to stop...\n";
    std::cin.get();

    spdlog::info("Main thread: Requesting worker stop...");
    worker.request_stop();

    shutdown_latch->wait();
    spdlog::info("Main thread: Worker has shut down. Exiting.");


    co_return 0;
}

int main() { sync_wait(main_work()); }
