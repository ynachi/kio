//
// tcp_next.cpp - TCP Echo Server using Dual Queue Executor
//

#include "kio/next/io_uring_executor.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

#include <arpa/inet.h>
#include <async_simple/coro/Lazy.h>

using namespace async_simple::coro;
using namespace kio::next::v1;

// Simple awaiters wrapper for v1 namespace locally for the demo
namespace io::v1
{
    inline auto accept(IoUringExecutor* exec, int listen_fd, sockaddr* addr, socklen_t* addrlen)
    {
        return make_io_awaiter<int>(exec,
            [=](io_uring_sqe* sqe) { io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0); });
    }

    inline auto recv(IoUringExecutor* exec, int fd, void* buf, size_t len, int flags = 0)
    {
        return make_io_awaiter<ssize_t>(exec,
            [=](io_uring_sqe* sqe) { io_uring_prep_recv(sqe, fd, buf, len, flags); });
    }

    inline auto send(IoUringExecutor* exec, int fd, const void* buf, size_t len, int flags = 0)
    {
        return make_io_awaiter<ssize_t>(exec,
            [=](io_uring_sqe* sqe) { io_uring_prep_send(sqe, fd, buf, len, flags); });
    }
}

Lazy<void> handleClient(IoUringExecutor* executor, int client_fd)
{
    char buffer[4096];

    try
    {
        while (true)
        {
            ssize_t bytes_read = co_await io::v1::recv(executor, client_fd, buffer, sizeof(buffer));

            if (bytes_read <= 0) break;

            static const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 13\r\n"
                "\r\n"
                "Hello, World!";

            ssize_t bytes_sent = 0;
            while (bytes_sent < response.size())
            {
                ssize_t sent = co_await io::v1::send(executor, client_fd,
                    response.data() + bytes_sent, response.size() - bytes_sent);
                if (sent <= 0) throw std::runtime_error("Send failed");
                bytes_sent += sent;
            }
        }
    }
    catch (const std::exception& e)
    {
        // Error handling
    }

    close(client_fd);
}

Lazy<> acceptLoop(IoUringExecutor* executor, int listen_fd)
{
    size_t next_ctx = 0;
    const size_t num_cpus = executor->numThreads();

    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = co_await io::v1::accept(executor, listen_fd,
            reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_fd < 0) continue;

        // Explicitly schedule processing to a worker thread (Round Robin)
        size_t target_ctx = next_ctx++ % num_cpus;
        executor->scheduleOn(target_ctx, [executor, client_fd]() {
            handleClient(executor, client_fd).start([](auto&&) {});
        });
    }
}

int createListenSocket(uint16_t port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        throw std::system_error(errno, std::system_category(), "socket");

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        close(listen_fd);
        throw std::system_error(errno, std::system_category(), "bind");
    }

    if (listen(listen_fd, SOMAXCONN) < 0)
    {
        close(listen_fd);
        throw std::system_error(errno, std::system_category(), "listen");
    }

    std::cout << "Listening on port " << port << std::endl;
    return listen_fd;
}

int main()
{
    try
    {
        constexpr uint16_t PORT = 8080;

        IoUringExecutorConfig config;
        config.num_threads = 4;
        config.io_uring_entries = 4096;
        config.pin_threads = true;
        config.io_uring_flags = 0;

        IoUringExecutor executor(config);

        int listen_fd = createListenSocket(PORT);

        std::cout << "HTTP server (V1 - Dual Queue) started on port " << PORT
                  << " with " << config.num_threads << " threads." << std::endl;

        // Synchronization to keep main alive
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;

        // Schedule acceptLoop to run on thread 0
        executor.scheduleOn(0, [&executor, listen_fd, &mtx, &cv, &done]() {
            acceptLoop(&executor, listen_fd).start([&mtx, &cv, &done](auto&&) {
                std::lock_guard<std::mutex> lock(mtx);
                done = true;
                cv.notify_one();
            });
        });

        // Wait for completion
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&done] { return done; });

        close(listen_fd);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}