#include "kio/next/io_awaiters.h"
#include "kio/next/io_uring_executor.h"

#include <iostream>
#include <memory>
#include <string>

#include <arpa/inet.h>
#include <async_simple/coro/Collect.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

using namespace async_simple::coro;
using namespace kio;

/**
The Golden Rule for Async C++

Treat any "root" coroutine (one you launch via schedule, start, or detach) exactly like a std::thread.

Never pass references to stack variables or lambda captures.

Always pass by Value (copy/move) or use std::shared_ptr.
 *
/**

/**
 * @brief Handle a single client connection (echo server)
 */
Lazy<> handleClient(next::IoUringExecutor* executor, int client_fd, const std::string client_addr)
{
    std::cout << "Client connected: " << client_addr << std::endl;

    char buffer[4096];

    try
    {
        while (true)
        {
            // Receive data
            // The usage is identical, but under the hood, it creates a zero-alloc awaiter
            ssize_t bytes_read = co_await io::next::recv(executor, client_fd, buffer, sizeof(buffer));

            if (bytes_read <= 0)
            {
                // Connection closed (0) or error (<0)
                break;
            }

            // Echo back
            ssize_t bytes_sent = 0;
            while (bytes_sent < bytes_read)
            {
                ssize_t sent =
                    co_await io::next::send(executor, client_fd, buffer + bytes_sent, bytes_read - bytes_sent);
                if (sent <= 0)
                {
                    throw std::runtime_error("Send failed");
                }
                bytes_sent += sent;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error handling client " << client_addr << ": " << e.what() << std::endl;
    }

    close(client_fd);
    std::cout << "Client disconnected: " << client_addr << std::endl;
}

/**
 * @brief HTTP Handler for Benchmarking
 * Responds with a fixed "Hello World" to any request.
 */
Lazy<void> handleClientHttp(next::IoUringExecutor* executor, int client_fd)
{
    // Optimization: Disable logging for high-throughput benchmarks
    // std::cout << "Client connected: " << client_addr << std::endl;

    char buffer[4096];

    try
    {
        while (true)
        {
            // Receive data (we just consume it to clear the socket)
            ssize_t bytes_read = co_await io::next::recv(executor, client_fd, buffer, sizeof(buffer));

            if (bytes_read <= 0)
            {
                break;
            }

            // Fixed HTTP response
            // Note: In a real server you'd want to handle partial writes properly,
            // but for simple "Hello World" benchmarks, this usually sends in one go.
            static const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 13\r\n"
                "\r\n"
                "Hello, World!";

            ssize_t bytes_sent = 0;
            while (bytes_sent < response.size())
            {
                ssize_t sent = co_await io::next::send(executor, client_fd, response.data() + bytes_sent,
                                                       response.size() - bytes_sent);
                if (sent <= 0)
                {
                    throw std::runtime_error("Send failed");
                }
                bytes_sent += sent;
            }
        }
    }
    catch (const std::exception& e)
    {
        // std::cerr << "Error handling client " << client_addr << ": " << e.what() << std::endl;
    }

    close(client_fd);
    // std::cout << "Client disconnected: " << client_addr << std::endl;
}

/**
 * @brief Accept connections and handle them
 */
Lazy<> acceptLoop(next::IoUringExecutor* executor, int listen_fd)
{
    size_t next_ctx = 0;
    const size_t num_cpus = executor->numThreads();

    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Accept connection
        int client_fd =
            co_await kio::io::next::accept(executor, listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_fd < 0)
        {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        // Convert address to string
        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        std::string client_addr_str = std::string(addr_str) + ":" + std::to_string(ntohs(client_addr.sin_port));

        // 1) Schedule a lambda that starts the coroutine on whichever worker `schedule` picks.
        // TODO: Only 1 CPU used here, why ?
        // executor->schedule([executor, client_fd]() { handleClientHttp(executor, client_fd).start([](auto&&) {}); });

        // 2) Use via() to attach the coroutine to the executor and start it outside the pool.
        // TODO: Only 1 CPU used here, why ?
        handleClientHttp(executor, client_fd).via(executor).start([](auto&&) {});

        // 3) Use scheduleOn() to choose a specific worker and then start the coroutine.
        // TODO: only this actually use all the CPUs
        size_t target_ctx = next_ctx++ % num_cpus;
        executor->scheduleOn(target_ctx,
                             [executor, client_fd]() { handleClientHttp(executor, client_fd).start([](auto&&) {}); });

        // 4) Directly call start() on the Lazy without any scheduling.
        // Don’t call start() on a Lazy from outside the executor without via(); the coroutine will initially run on
        // your current thread and only switch to a worker after an await, leading to under‑utilisation.
        // handleClientHttp(executor, client_fd, client_addr_str).start([](auto&&) {});
    }
}

/**
 * @brief Create and bind a listening socket
 */
int createListenSocket(uint16_t port, int backlog = 128)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        throw std::system_error(errno, std::system_category(), "Failed to create socket");
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        close(listen_fd);
        throw std::system_error(errno, std::system_category(), "Failed to bind");
    }

    if (listen(listen_fd, backlog) < 0)
    {
        close(listen_fd);
        throw std::system_error(errno, std::system_category(), "Failed to listen");
    }

    std::cout << "Listening on port " << port << std::endl;
    return listen_fd;
}

int main()
{
    try
    {
        constexpr uint16_t PORT = 8080;

        // Create executor
        next::IoUringExecutorConfig config;
        // config.num_threads = std::thread::hardware_concurrency();
        config.num_threads = 4;
        config.io_uring_entries = 16800;
        config.pin_threads = true;
        // config.io_uring_flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
        // config.io_uring_flags = IORING_SETUP_SQPOLL;

        next::IoUringExecutor executor(config);

        int listen_fd = createListenSocket(PORT);

        std::cout << "Echo server started on port " << PORT << " with " << config.num_threads << " threads."
                  << std::endl;
        std::cout << "Run 'echo Hello | nc localhost " << PORT << "' to test." << std::endl;

        // Start the acceptance loop
        // We start it via the executor to ensure the coroutine runs in the executor context
        syncAwait(acceptLoop(&executor, listen_fd).via(&executor));

        close(listen_fd);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}