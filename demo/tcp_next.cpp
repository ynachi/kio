//
// tcp_next.cpp - TCP Echo Server (Multi-Reactor Pattern + SO_REUSEPORT)
//

#include "kio/next/io_uring_executor.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <async_simple/coro/Lazy.h>

using namespace async_simple::coro;
using namespace kio::next::v1;

// Simple awaiters wrapper
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
            // Read
            ssize_t bytes_read = co_await io::v1::recv(executor, client_fd, buffer, sizeof(buffer));
            if (bytes_read <= 0) break;

            static const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 13\r\n"
                "\r\n"
                "Hello, World!";

            // Write
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
    catch (...) {}
    close(client_fd);
}

//
// Accept Loop: Now runs per-thread.
// No cross-thread scheduling required. Connections are handled where they arrive.
//
Lazy<> acceptLoop(IoUringExecutor* executor, int listen_fd)
{
    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = co_await io::v1::accept(executor, listen_fd,
            reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_fd >= 0)
        {
            // Spawn handler locally on this thread.
            // Since we are already in the correct executor context, .start() simply
            // queues the coroutine to run on this thread (sticky scheduling).
            handleClient(executor, client_fd).start([](auto&&) {});
        }
    }
}

int createListenSocket(uint16_t port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        throw std::system_error(errno, std::system_category(), "socket");

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // CRITICAL for Multi-Reactor: Allow multiple sockets to bind to same port
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

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

    return listen_fd;
}

int main()
{
    try
    {
        constexpr uint16_t PORT = 8080;

        IoUringExecutorConfig config;
        config.num_threads = 4;
        config.io_uring_entries = 32768;
        config.pin_threads = true;
        config.io_uring_flags = 0;

        IoUringExecutor executor(config);

        std::cout << "HTTP server (Multi-Reactor + SO_REUSEPORT) started on port " << PORT
                  << " with " << config.num_threads << " threads." << std::endl;

        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;

        // Create N listeners for N threads
        // Each thread gets its own socket and its own accept loop
        std::vector<int> listen_fds;
        for(size_t i = 0; i < config.num_threads; ++i)
        {
            int fd = createListenSocket(PORT);
            listen_fds.push_back(fd);

            // Schedule the accept loop specifically on thread 'i'
            executor.scheduleOn(i, [&executor, fd, &mtx, &cv, &done]() {
                acceptLoop(&executor, fd).start([&mtx, &cv, &done](auto&&) {
                    // In a real server we wouldn't exit, but here we keep the structure
                    std::lock_guard<std::mutex> lock(mtx);
                    done = true;
                    cv.notify_one();
                });
            });
        }

        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&done] { return done; });

        for(int fd : listen_fds) close(fd);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}