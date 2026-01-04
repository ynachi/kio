//
// Created by Yao ACHI on 18/10/2025.
//

#include "kio/core/coro.h"
#include "kio/core/worker.h"
#include "kio/net/net.h"
#include "kio/sync/sync_wait.h"

#include <iostream>

using namespace kio;
using namespace kio::io;
using namespace kio::net;

// Client processing code, which will run in the background on the same thread as the worker.
// Thus, there is no need to context switch as we are already on the right thread.
// The loop is sync to the worker's stop signal for a coordinated shutdown.
DetachedTask HandleClient(Worker& worker, const int client_fd)
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
            ALOG_INFO("Read failed {}", n.error());
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
            ALOG_ERROR("Write failed: {}", sent.error());
            break;
        }
    }

    close(client_fd);
}

// 2. accept_loop is an awaitable Task<void>.
Task<void> accept_loop(Worker& worker, int listen_fd)
{
    ALOG_INFO("Worker accepting connections");
    const auto st = worker.GetStopToken();

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Before we do anything that touches the worker's io_uring ring,
        // we must switch execution to the worker's thread. Running in debug mode
        // would verify this behavior and crash
        co_await SwitchToWorker(worker);

        auto client_fd = co_await worker.AsyncAccept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (!client_fd.has_value())
        {
            // If a stop is requested, exit
            if (st.stop_requested())
            {
                break;
            }
            ALOG_ERROR("Accept failed: {}", client_fd.error());
            continue;
        }

        ALOG_DEBUG("Accepted connection on fd {}", client_fd.value());

        // This is still "fire and forget"
        HandleClient(worker, client_fd.value());
    }

    ALOG_INFO("Worker {} stop accepting connexions", worker.GetId());
    co_return;
}

int main()
{
    // 1. Application configuration
    signal(SIGPIPE, SIG_IGN);
    alog::Configure(1024, LogLevel::kInfo);

    const std::string ip_address = "127.0.0.1";
    constexpr int port = 8080;

    auto server_fd = create_tcp_server_socket(ip_address, port, 128);
    if (!server_fd)
    {
        // Assuming IoErrorToString exists
        ALOG_ERROR("Failed to create server socket: {}", server_fd.error());
        return 1;
    }
    ALOG_INFO("server listening on endpoint: {}:{}, FD:{}", ip_address, port, server_fd.value());

    // 2. Worker setup
    WorkerConfig config{};
    config.uring_queue_depth = 2048;

    Worker worker(0, config);

    // 3. Start the event loop in a background thread, so that
    // the main can run the rest of the code
    auto thread = std::jthread([&worker] { worker.LoopForever(); });
    ALOG_INFO("Main thread: Waiting for worker to initialize...");

    // 4. Important! wait for the worker to fully start
    worker.WaitReady();
    ALOG_INFO("Main thread: Worker is ready.");

    // now start listening to clients
    // The code blocks here so the lines after will not run.
    // Look at the V2 code to see how to "solve" that issue.
    // Spoiler: Just use a detached coroutine.
    SyncWait(accept_loop(worker, server_fd.value()));

    std::cout << "Server listening on 127.0.0.1:8080. Press Enter to stop...\n";
    std::cin.get();

    ALOG_INFO("Main thread: Requesting worker stop...");
    if (const auto res = worker.RequestStop(); !res)
    {
        ALOG_ERROR("failed to request the event loop to stop");
        return 1;
    }

    ALOG_INFO("Main thread: Worker has shut down. Exiting.");

    return 0;
}
