#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include "aio/aio.hpp"
#include <netinet/in.h>
#include <netinet/tcp.h>

// Global counters
static std::atomic<uint64_t> g_requests{0};
static std::atomic<uint64_t> g_connections{0};

static std::atomic g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

static void ignore_sigpipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

static int set_nodelay(int fd) {
    int one = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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

// -----------------------------------------------------------------------------
// Client handler (one coroutine per connection)
// -----------------------------------------------------------------------------

static aio::task<> handle_client(aio::io_context& ctx, int fd) {
    (void)set_nodelay(fd);

    alignas(64) char buffer[4096];

    static constexpr std::string_view response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, World!";

    while (true) {
        auto rr = co_await aio::async_recv(&ctx, fd, buffer, sizeof(buffer), 0);
        if (!rr) {
            break;
        }
        if (*rr == 0) {
            break; // EOF
        }

        g_requests.fetch_add(1, std::memory_order_relaxed);

        size_t sent = 0;
        while (sent < response.size()) {
            const char* p = response.data() + sent;
            size_t n = response.size() - sent;
            auto sr = co_await aio::async_send(&ctx, fd, p, n, 0);
            if (!sr) {
                goto out;
            }
            if (*sr == 0) {
                goto out;
            }
            sent += *sr;
        }
    }

out:
    // Best-effort async close; ignore errors.
    (void)co_await aio::async_close(&ctx, fd);
    co_return;
}

// -----------------------------------------------------------------------------
// Accept loop (one per worker)
// -----------------------------------------------------------------------------

static aio::task<void> accept_loop(aio::io_context& ctx, int listen_fd,
                                  std::vector<aio::task<>>& connections) {
    uint64_t sweep_counter = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        auto ar = co_await aio::async_accept(&ctx, listen_fd);
        if (!ar) {
            int e = ar.error().value();
            if (e == EBADF || e == ECANCELED) {
                break;
            }
            if (e == EMFILE || e == ENFILE) {
                (void)co_await aio::async_sleep(&ctx, std::chrono::milliseconds(10));
            }
            continue;
        }

        int client_fd = *ar;
        g_connections.fetch_add(1, std::memory_order_relaxed);

        // Start client coroutine and keep ownership in the vector.
        auto t = handle_client(ctx, client_fd);
        t.resume();
        connections.push_back(std::move(t));

        // Periodic sweep of completed connections to avoid unbounded growth.
        if ((++sweep_counter & 0x3FFu) == 0) { // every 1024 accepts
            // erase done tasks (C++23 erase_if on vector)
            std::erase_if(connections, [](aio::task<void>& x) { return x.done(); });
        }
    }

    co_return;
}

// -----------------------------------------------------------------------------
// Worker container
// -----------------------------------------------------------------------------

struct worker {
    std::unique_ptr<aio::io_context> ctx;
    int ring_fd = -1;
    std::thread th;
};

// Send WAKE_TAG CQE to target ring to break submit_and_wait.
static void wake_ring(io_uring& waker, int target_ring_fd) {
    io_uring_sqe* sqe = io_uring_get_sqe(&waker);
    if (!sqe) {
        io_uring_submit(&waker);
        sqe = io_uring_get_sqe(&waker);
        if (!sqe) return;
    }

    io_uring_prep_msg_ring(sqe, target_ring_fd, /*res*/ 0u, /*user_data*/ aio::WAKE_TAG, 0);
    io_uring_sqe_set_data(sqe, nullptr);
}

int main(int argc, char** argv) {
    ignore_sigpipe();
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const uint16_t port = (argc >= 2) ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;
    const int num_threads = (argc >= 3) ? std::max(1, std::atoi(argv[2])) : 4;

    int listen_fd = make_listen_socket(port);
    if (listen_fd < 0) {
        std::perror("bind/listen");
        return 1;
    }

    std::fprintf(stderr, "Server started on port %u (Threads: %d)\n", port, num_threads);

    // Create workers (io_context objects live in main so we can stop() them).
    std::vector<worker> workers;
    workers.reserve(static_cast<size_t>(num_threads));

    for (int i = 0; i < num_threads; ++i) {
        worker w;
        w.ctx = std::make_unique<aio::io_context>(/*entries=*/4096);
        w.ring_fd = w.ctx->ring_fd();
        workers.push_back(std::move(w));
    }

    // Start worker threads.
    for (int i = 0; i < num_threads; ++i) {
        worker& w = workers[static_cast<size_t>(i)];
        w.th = std::thread([&, i] {
            // All coroutine submission happens on this worker thread.
            std::vector<aio::task<void>> conns;
            conns.reserve(1024);

            auto acc = accept_loop(*w.ctx, listen_fd, conns);
            acc.resume();

            w.ctx->run();

            // Ensure all in-flight ops are untracked before destroying tasks.
            w.ctx->cancel_all_pending();
            // Now conns + acc destruct safely.
        });
    }

    // Monitoring loop
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto conns = g_connections.exchange(0, std::memory_order_relaxed);
        auto reqs  = g_requests.exchange(0, std::memory_order_relaxed);
        if (conns || reqs) {
            std::fprintf(stderr, "[Stats] New Conns: %llu, Reqs: %llu\n",
                         (unsigned long long)conns, (unsigned long long)reqs);
        }
    }

    std::fprintf(stderr, "Shutting down...\n");

    // Stop accepting new connections.
    ::close(listen_fd);

    // Ask workers to stop and wake them if they're blocked in submit_and_wait.
    io_uring waker;
    std::memset(&waker, 0, sizeof(waker));
    int ret = io_uring_queue_init(64, &waker, 0);
    if (ret < 0) {
        std::fprintf(stderr, "waker ring init failed: %d\n", ret);
    } else {
        for (auto& w : workers) {
            w.ctx->stop();
            wake_ring(waker, w.ring_fd);
        }
        io_uring_submit(&waker);

        // Drain waker CQEs (best-effort)
        io_uring_cqe* cqe;
        unsigned head, count = 0;
        io_uring_for_each_cqe(&waker, head, cqe) { ++count; }
        if (count) io_uring_cq_advance(&waker, count);

        io_uring_queue_exit(&waker);
    }

    for (auto& w : workers) {
        if (w.th.joinable()) w.th.join();
    }

    std::fprintf(stderr, "Bye.\n");
    return 0;
}
