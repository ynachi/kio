// This is not an echo server. It instead return a static http formated response.
// Useful for benchmarking.
//
// Run:
//   ./echo                                 # :8080, 4 threads, 30s idle timeout
//   ./echo --port=9000 --threads=8
//   ./echo --idle-timeout=5s --sqpoll
//   ./echo --help                          # auto-generated flag list
//   ./echo --flagfile=prod.flags           # bulk config from a file
//
// Poke it:
//   curl -v http://127.0.0.1:8080/
//   curl -v --http1.1 -H 'Connection: close' http://127.0.0.1:8080/
//   printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | nc 127.0.0.1 8080

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <cstdlib>
#include <print>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <liburing.h>

#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/time/time.h>

#include "kio/context.h"
#include "kio/operations.h"
#include "kio/task.h"
#include "kio/tcp_listener.hpp"

using namespace kio;
using namespace std::chrono_literals;

// ===========================================================================
// FLAGS -- must live in the GLOBAL namespace, not the anonymous one.
// ABSL_FLAG emits a `FLAGS_<name>` symbol that other translation units need
// to address via ABSL_DECLARE_FLAG / absl::GetCommandLineFlag().
// ===========================================================================

ABSL_FLAG(uint16_t, port, 8080, "TCP port to listen on (each thread gets its own SO_REUSEPORT socket).");
ABSL_FLAG(int, threads, 4, "Number of IO threads; each owns its Context and listener, nothing is shared.");
ABSL_FLAG(absl::Duration, idle_timeout, absl::Seconds(30),
          "Drop a connection idle this long, via a linked io_uring timeout. Accepts '30s', '500ms', '2m'.");
ABSL_FLAG(std::uint32_t, uring_entries, 4096, "io_uring SQ/CQ depth (IoOptions::entries).");
ABSL_FLAG(std::size_t, batch_size, 128, "Max coroutine resumes per run-loop epoch (IoOptions::batch_max_size).");
ABSL_FLAG(bool, sqpoll, false,
          "Enable IORING_SETUP_SQPOLL (kernel submission thread). Needs CAP_SYS_NICE on most kernels; "
          "DEFER_TASKRUN/COOP_TASKRUN are dropped automatically because the kernel rejects that pair.");


namespace
{
    // Resolved configuration. Copied by value into each worker thread, so the
    // coroutines read plain ints/chrono values -- no flag-lock traffic per IO.
    struct ServerConfig
    {
        uint16_t port{};
        std::chrono::nanoseconds idle_timeout{};
        io::IoOptions io_opts{};
    };

    constexpr std::string_view kHttpResponse =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 16\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, io_uring!";

    Task<> handle_client(io::Context& io, io::UniqueFd fd, const std::chrono::nanoseconds idle_timeout)
    {
        // lives in the coroutine frame across every co_await
        std::byte buf[4096];
        for (;;)
        {
            auto n = co_await async_recv(io, fd, std::span{buf}).timeout(idle_timeout);
            if (!n || *n == 0)
            {
                break; // error, idle timeout (ETIMEDOUT) or EOF
            }
            auto w = co_await async_send(
                io, fd,
                std::span{
                    reinterpret_cast<const std::byte*>(kHttpResponse.data()),
                    kHttpResponse.size()
                },
                MSG_NOSIGNAL);
            if (!w || *w == 0) break;
        }
        co_await async_close(io, std::move(fd));

        co_return {};
    }

    Task<> acceptor(io::Context& io, const io::UniqueFd& lfd, const std::chrono::nanoseconds idle_timeout)
    {
        for (;;)
        {
            auto c = co_await async_accept(io, lfd);
            if (!c)
            {
                const Error& e = c.error();
                if (e == std::errc::operation_canceled)
                {
                    break; // shutdown: the ONLY sanctioned exit, never retry
                }
                if (e.value() == EMFILE || e.value() == ENFILE)
                {
                    std::println(stderr, "accept: {}", e.message());
                    co_await async_sleep(io, 100ms); // if canceled, next accept returns ECANCELED -> break
                    continue;
                }
                if (e.value() == ECONNABORTED || e.value() == EINTR)
                {
                    continue; // transient, per-connection
                }
                LOG(ERROR) << "accept failed fatally: " << e.message();
                co_return std::unexpected(e);
            }
            io.spawn(handle_client(io, io::UniqueFd(*c), idle_timeout));
        }
        co_return {};
    }

    void serve(std::stop_token st, const ServerConfig& cfg, const int id)
    {
        io::Context io(static_cast<std::size_t>(id), /*leader=*/nullptr, cfg.io_opts);
        auto lfd = TcpListener::bind(cfg.port, "127.0.0.1", 128);
        if (!lfd.has_value())
        {
            LOG(FATAL) << "Failed to bind to port " << cfg.port;
        }
        io.spawn(acceptor(io, *lfd, cfg.idle_timeout));
        io.run(st); // returns once quiesced after the stop-triggered drain
    }
} // namespace

int main(const int argc, char** argv)
{
    absl::ParseCommandLine(argc, argv);


    ServerConfig cfg;
    cfg.port = absl::GetFlag(FLAGS_port);
    cfg.idle_timeout = absl::ToChronoNanoseconds(absl::GetFlag(FLAGS_idle_timeout));
    cfg.io_opts.entries = absl::GetFlag(FLAGS_uring_entries);
    cfg.io_opts.batch_max_size = absl::GetFlag(FLAGS_batch_size);


    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    const int threads = absl::GetFlag(FLAGS_threads);
    LOG(INFO) << std::format("echo listening on 127.0.0.1:{} with {} thread(s), idle timeout {}ms",
                             cfg.port, threads,
                             std::chrono::duration_cast<std::chrono::milliseconds>(cfg.idle_timeout).count());

    std::vector<std::jthread> ts;
    ts.reserve(static_cast<std::size_t>(threads)); // was threads - 1
    for (int i = 0; i < threads; ++i)
    {
        ts.emplace_back(serve, cfg, i); // jthread supplies its own stop_token as arg 0
    }

    int sig = 0;
    sigwait(&sigs, &sig);
    LOG(INFO) << "signal " << sig << ", shutting down";

    for (auto& t : ts) t.request_stop(); // all workers drain in parallel
    ts.clear(); // joins; each run() returns at quiescence

    LOG(INFO) << "server stopped";
}
