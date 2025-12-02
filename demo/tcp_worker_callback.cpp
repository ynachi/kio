//
// Created by Yao ACHI on 17/10/2025.
//

/**
 * Demonstrate the usage of an IO worker with a detached coroutine callback.
 **/

#include <iostream>

#include "kio/include/async_logger.h"
#include "kio/include/io/metrics.h"
#include "kio/include/io/worker.h"
#include "kio/include/metrics/registry.h"
#include "kio/include/net.h"


using namespace kio;
using namespace kio::io;
using namespace kio::net;

// User defines their application logic as coroutines
DetachedTask HandleClient(Worker &worker, const int client_fd)
{
    char buffer[8192];
    const auto st = std::stop_token{};

    while (!st.stop_requested())
    {
        // Read from the client - this co_await runs on the worker thread
        auto n = co_await worker.async_read(client_fd, std::span(buffer, sizeof(buffer)));
        if (!n.has_value())
        {
            ALOG_DEBUG("Read failed {}", n.error());
            // in the io_uring world, most of the errors are fatal, so no need to specialize
            break;
        }

        if (n.value() == 0)
        {
            ALOG_INFO("Client disconnected");
            break;
        }

        // Process data (parse HTTP, handle request, etc.)
        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";

        // Write response - this co_await also runs on the worker thread
        auto sent = co_await worker.async_write(client_fd, std::span(response.data(), response.size()));

        if (!sent.has_value())
        {
            ALOG_ERROR("Write failed: {}", sent.error());
            break;
        }
    }

    close(client_fd);
}


// Accept loop - runs on each worker independently
DetachedTask accept_loop(Worker &worker, int listen_fd)
{
    ALOG_INFO("Worker accepting connections");
    const auto st = std::stop_token{};

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Accept connection - blocks this coroutine until client connects
        auto client_fd = co_await worker.async_accept(listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &addr_len);

        if (!client_fd.has_value())
        {
            ALOG_ERROR("Accept failed: {}", client_fd.error());
            continue;
        }

        ALOG_INFO("Accepted connection on fd {}", client_fd.value());

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        HandleClient(worker, client_fd.value()).detach();
    }
    ALOG_INFO("Worker {} stop accepting connexions", worker.get_id());
}

int main()
{
    // ignore signals
    signal(SIGPIPE, SIG_IGN);
    // Setup logging
    alog::configure(1024, LogLevel::Info);

    // create a server socket
    const std::string ip_address = "127.0.0.1";
    constexpr int port = 8080;

    auto server_fd = create_tcp_socket(ip_address, port, 128);
    if (!server_fd)
    {
        ALOG_ERROR("Failed to create server socket: {}", server_fd.error());
        return 1;
    }
    ALOG_INFO("server listening on endpoint: {}:{}, FD:{}", ip_address, port, server_fd.value());

    WorkerConfig config{};
    config.uring_queue_depth = 2048;
    config.default_op_slots = 4096;

    std::stop_source stop_source;

    // Here we create the worker and assign the accept_loop coroutine to it. This is one way to run
    // our io operations in the worker thread. Because that function runs directly in the worker, we don't need to use
    // the SwitchToWorker mechanism. Also notes that workers are lazy. Here the worker is created, but the event loop
    // is not started yet.
    Worker worker(0, config, [server_fd](Worker &worker) { accept_loop(worker, server_fd.value()).detach(); });

    // register metrics collector
    auto &registry = MetricsRegistry<>::Instance();
    const auto worker_collector = std::make_shared<WorkerMetricsCollector>(worker);
    registry.Register(worker_collector);

    ALOG_INFO("Main thread: Waiting for worker to initialize...");

    // Start the worker in a thread. This is useful because loop_forever is a blocking method.
    // If we want to execute the rest of this code, whose purpose is to control how we stop
    // the worker, we need to do that.
    std::jthread t([&]() { worker.loop_forever(); });

    ALOG_INFO("Main thread: Worker is ready.");

    // Block here, wait for the user input to progress.
    ALOG_INFO("Server listening on 127.0.0.1:8080. Press Enter to stop...");
    std::cin.get();

    // print metrics
    std::cout << std::format("worker metrics: {}", registry.Scrape());
    // Request the worker to stop.
    ALOG_INFO("Main thread: Requesting worker stop...");
    if (const auto ret = worker.request_stop(); !ret)
    {
        ALOG_ERROR("failed to request the event loop to stop");
        ::close(server_fd.value());
        std::exit(1);
    }
    // This signals the thread and drains completions

    // 9. Wait for the worker to fully shut down
    ::close(server_fd.value());
    ALOG_INFO("Main thread: Worker has shut down. Exiting.");

    return 0;
}
