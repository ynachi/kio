////////////////////////////////////////////////////////////////////////////////
// benchmark_server.cpp - High-performance HTTP Benchmark Server
//
// Updates:
// 1. Clean shutdown logic added (Fixes Leak & Crash)
// 2. Uses signal handling for graceful stop
//
// Build: g++ -std=c++23 -O3 -o benchmark_server benchmark_server.cpp -luring -pthread
////////////////////////////////////////////////////////////////////////////////

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#include <sys/socket.h>

#include "../runtime.hpp"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <utilities/kio_logger.hpp>

using namespace uring;

// Global debug counters
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_connections{0};

// Helper to set socket options for performance
void tune_socket(int fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    int bufsize = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
}

// A fire-and-forget coroutine that destroys itself upon completion.
struct DetachedTask {
    struct promise_type {
        DetachedTask get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

Task<> handleClientLogic(Executor& ex, int client_fd) {
    tune_socket(client_fd);
    char buffer[4096];

    static const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, World!";

    try {
        while (true) {
            Result<int> read_res;
            while (true) {
                read_res = co_await recv(ex, client_fd, buffer, sizeof(buffer));
                if (!read_res) {
                    if (read_res.error() == EAGAIN || read_res.error() == EWOULDBLOCK) {
                        co_await timeout(ex, 0);
                        continue;
                    }
                    goto cleanup;
                }
                break;
            }

            if (*read_res <= 0) break;

            g_requests.fetch_add(1, std::memory_order_relaxed);

            size_t bytes_sent = 0;
            while (bytes_sent < response.size()) {
                auto send_res = co_await send(ex, client_fd,
                                            response.data() + bytes_sent,
                                            response.size() - bytes_sent);
                if (!send_res) {
                    if (send_res.error() == EAGAIN || send_res.error() == EWOULDBLOCK) {
                        co_await timeout(ex, 0);
                        continue;
                    }
                    goto cleanup;
                }
                bytes_sent += *send_res;
            }
        }
    } catch (...) {}

cleanup:
    co_await close(ex, client_fd);
}

DetachedTask launchClientHandler(Executor& ex, int client_fd) {
    co_await handleClientLogic(ex, client_fd);
}

Task<> acceptLoop(Executor& ex, int listen_fd) {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        auto res = co_await accept(ex, listen_fd,
                                 reinterpret_cast<sockaddr*>(&client_addr),
                                 &addr_len);

        if (!res) {
            int err = res.error();
            // Retry on EAGAIN
            if (err == EAGAIN || err == EWOULDBLOCK) {
                 co_await timeout(ex, 0);
                 continue;
            }
            // Backoff on file limit
            if (err == EMFILE || err == ENFILE) {
                co_await timeout_ms(ex, 10);
                continue;
            }
            // CRITICAL: Exit loop on cancellation/bad FD (Shutdown triggered)
            if (err == ECANCELED || err == EBADF || err == EINVAL) {
                break;
            }
            continue;
        }

        int client_fd = *res;
        g_connections.fetch_add(1, std::memory_order_relaxed);
        launchClientHandler(ex, client_fd);
    }
}

int createListenSocket(uint16_t port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) throw std::system_error(errno, std::system_category(), "socket");

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd);
        throw std::system_error(errno, std::system_category(), "bind");
    }

    if (listen(listen_fd, 4096) < 0) {
        ::close(listen_fd);
        throw std::system_error(errno, std::system_category(), "listen");
    }

    return listen_fd;
}

std::atomic g_running{true};

void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

int main() {
    Log::g_level = Log::Level::Info;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        constexpr uint16_t PORT = 8080;
        RuntimeConfig config;
        config.num_threads = 4;
        config.pin_threads = true;
        config.executor_config.entries = 32768;

        Runtime rt(config);
        int listen_fd = createListenSocket(PORT);

        std::cout << "Server on port " << PORT << " (Threads: " << config.num_threads << ")\n";

        rt.loop_forever(config.pin_threads);

        for (size_t i = 0; i < rt.size(); ++i) {
            auto& ctx = rt.thread(i);
            ctx.schedule(acceptLoop(ctx.executor(), listen_fd));
        }

        std::cout << "Running. Press Ctrl+C to stop.\n";

        // Main monitor loop
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t conns = g_connections.exchange(0, std::memory_order_relaxed);
            uint64_t reqs = g_requests.exchange(0, std::memory_order_relaxed);
            if (conns > 0 || reqs > 0) {
                Log::info("[Stats] New Conns: {}, Reqs: {}", conns, reqs);
            }
        }

        // --- CLEAN SHUTDOWN SEQUENCE ---
        Log::info("Shutting down...");

        // 1. Close the listen socket.
        // This causes pending accept() calls in workers to return -ECANCELED or -EBADF.
        // The acceptLoop() will catch this error, break the loop, and return.
        // The FireAndForgetTask will then destroy the coroutine frame (fixing the leak).
        shutdown(listen_fd, SHUT_RDWR);
        ::close(listen_fd);

        // 2. Allow a brief moment for workers to process the cancellation events.
        // If we stop the runtime immediately, the threads might exit before processing the CANCELED event.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 3. Stop the runtime (joins threads)
        rt.stop();
        std::cout << "Bye." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}