//
// tcp_demo_affinity.cpp - Real working example with connection affinity
//

#include "kio/next/io_awaiters.h"
#include "kio/next/io_uring_executor.h"

#include <iostream>

#include <arpa/inet.h>
#include <async_simple/coro/Lazy.h>

using namespace async_simple::coro;
using namespace kio::next::v1;
using namespace kio::next;

using kio::io::next::recv;
using kio::io::next::send;
using kio::io::next::read;
using kio::io::next::accept;

struct Connection {
    int fd;
    size_t affinity_lane;  // All I/O for this connection on same CPU
};

Lazy<void> handleClientWithAffinity(IoUringExecutor* executor, Connection conn)
{
    std::cout << "[Lane " << conn.affinity_lane << "] Handling client fd=" << conn.fd << std::endl;

    char buffer[4096];

    try {
        while (true) {
            // All I/O for this connection stays on same lane = cache hot!
            ssize_t n = co_await recv(executor, conn.fd, buffer, sizeof(buffer))
                .on_context(conn.affinity_lane);

            if (n <= 0) break;

            static const char response[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 13\r\n"
                "\r\n"
                "Hello, World!";

            co_await send(executor, conn.fd, response, sizeof(response) - 1)
                .on_context(conn.affinity_lane);
        }
    } catch (...) {}

    close(conn.fd);
    std::cout << "[Lane " << conn.affinity_lane << "] Client fd=" << conn.fd << " closed" << std::endl;
}

Lazy<> acceptLoop(IoUringExecutor* executor, int listen_fd)
{
    size_t next_conn = 0;
    const size_t num_lanes = executor->numThreads();

    while (true) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = co_await accept(executor, listen_fd,
            reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_fd < 0) continue;

        // Choose lane based on fd hash for consistent affinity
        Connection conn;
        conn.fd = client_fd;
        conn.affinity_lane = std::hash<int>{}(client_fd) % num_lanes;

        std::cout << "New connection fd=" << client_fd
                  << " -> lane " << conn.affinity_lane << std::endl;

        // Schedule on chosen lane
        executor->scheduleOn(conn.affinity_lane, [executor, conn]() {
            handleClientWithAffinity(executor, conn).start([](auto&&) {});
        });
    }
}

int main()
{
    IoUringExecutorConfig config;
    config.num_threads = 4;
    config.io_uring_entries = 4096;
    config.pin_threads = true;
    config.io_uring_flags = IORING_SETUP_SQPOLL;

    IoUringExecutor executor(config);

    // Create listen socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 4096);

    std::cout << "=== Connection Affinity Demo ===" << std::endl;
    std::cout << "Server listening on port 8080" << std::endl;
    std::cout << "Each connection pinned to specific lane (cache locality)" << std::endl;
    std::cout << std::endl;

    // Start accept loop
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    executor.scheduleOn(0, [&]() {
        acceptLoop(&executor, listen_fd).start([&](auto&&) {
            std::lock_guard<std::mutex> lock(mtx);
            done = true;
            cv.notify_one();
        });
    });

    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return done; });

    close(listen_fd);
    return 0;
}