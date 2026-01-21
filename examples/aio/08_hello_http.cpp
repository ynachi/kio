// HTTP "Hello World" benchmark server
// Demonstrates: async_accept/recv/send/close, task lifetime container + sweeping, clean shutdown via ring_waker.
// Notes: ignores SIGPIPE; handles partial sends.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>
#include <algorithm>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aio/aio.hpp"

static std::atomic<bool>     g_running{true};
static std::atomic<uint64_t> g_connections{0};
static std::atomic<uint64_t> g_requests{0};

static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }
static void ignore_sigpipe() { std::signal(SIGPIPE, SIG_IGN); }

static void set_nodelay(int fd) {
    int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static int make_listen_socket(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 1024) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static aio::task<void> handle_client(aio::io_context& ctx, int fd) {
    set_nodelay(fd);

    alignas(64) char buf[4096];

    static constexpr std::string_view resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, World!";

    while (true) {
        auto rr = co_await aio::async_recv(&ctx, fd, buf, sizeof(buf), 0);
        if (!rr || *rr == 0) break;

        g_requests.fetch_add(1, std::memory_order_relaxed);

        size_t sent = 0;
        while (sent < resp.size()) {
            auto sr = co_await aio::async_send(&ctx, fd, resp.data() + sent, resp.size() - sent, 0);
            if (!sr || *sr == 0) goto out;
            sent += *sr;
        }
    }

out:
    (void)co_await aio::async_close(&ctx, fd);
    co_return;
}

static aio::task<void> accept_loop(aio::io_context& ctx, int listen_fd,
                                  std::vector<aio::task<void>>& conns) {
    uint64_t sweep = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        auto ar = co_await aio::async_accept(&ctx, listen_fd);
        if (!ar) {
            const int e = ar.error().value();
            if (e == EBADF || e == ECANCELED) break;

            if (e == EMFILE || e == ENFILE) {
                (void)co_await aio::async_sleep(&ctx, std::chrono::milliseconds(10));
            }
            continue;
        }

        g_connections.fetch_add(1, std::memory_order_relaxed);

        // SAFETY: keep task alive in container until done
        auto t = handle_client(ctx, *ar);
        t.start();
        conns.push_back(std::move(t));

        // periodic sweep to avoid unbounded growth
        if ((++sweep & 0x3FFu) == 0) {
            std::erase_if(conns, [](aio::task<void>& t) { return t.done(); });
        }
    }

    co_return;
}

struct Worker {
    std::unique_ptr<aio::io_context> ctx;
    std::thread th;
};

int main(int argc, char** argv) {
    ignore_sigpipe();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const uint16_t port = (argc >= 2) ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;
    const int threads = (argc >= 3) ? std::max(1, std::atoi(argv[2])) : 4;

    int listen_fd = make_listen_socket(port);
    if (listen_fd < 0) {
        std::perror("bind/listen");
        return 1;
    }

    std::fprintf(stderr, "http_hello: port=%u threads=%d\n", port, threads);

    std::vector<Worker> workers;
    workers.reserve(static_cast<size_t>(threads));

    for (int i = 0; i < threads; ++i) {
        Worker w;
        w.ctx = std::make_unique<aio::io_context>(4096);
        workers.push_back(std::move(w));
    }

    for (int i = 0; i < threads; ++i) {
        Worker& w = workers[static_cast<size_t>(i)];
        w.th = std::thread([&, i] {
            std::vector<aio::task<void>> conns;
            conns.reserve(1024);

            auto acc = accept_loop(*w.ctx, listen_fd, conns);
            acc.start();

            w.ctx->run();
            w.ctx->cancel_all_pending();
        });
    }

    // stats
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto c = g_connections.exchange(0, std::memory_order_relaxed);
        auto r = g_requests.exchange(0, std::memory_order_relaxed);
        if (c || r) std::fprintf(stderr, "[stats] conns=%llu reqs=%llu\n",
                                 (unsigned long long)c, (unsigned long long)r);
    }

    std::fprintf(stderr, "shutdown...\n");

    ::close(listen_fd);

    // Clean shutdown using ring_waker
    aio::ring_waker waker;
    for (auto& w : workers) {
        w.ctx->stop();
        waker.wake(*w.ctx);
    }

    for (auto& w : workers) {
        if (w.th.joinable()) w.th.join();
    }

    std::fprintf(stderr, "bye.\n");
    return 0;
}