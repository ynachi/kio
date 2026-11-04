// Throwaway bench server: normal vs registered-buffer socket I/O.
// Usage: bench_net_server <normal|fixed> <http|echo> <port> <threads>
//   http: the established 16-byte-response pattern (oha-driven, -c 150)
//   echo: 64 KiB request -> echo response (bench_net_client-driven)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

#include "kio/buffer_pool.hpp"
#include "kio/context.h"
#include "kio/operations.h"
#include "kio/task.h"

using namespace kio;
using namespace std::chrono_literals;

namespace
{
    constexpr std::string_view kHttpResponse =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 16\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, io_uring!";
    constexpr unsigned kEchoMsg = 65536;

    // Echo mode: stream reads return short reads, so fill the full 64 KiB
    // message before echoing, and loop the echo (sends can short-write too).
    Task<> handle_client(io::Context& io, io::UniqueFd fd, const bool fixed)
    {
        std::byte heap_buf[kEchoMsg];
        for (;;)
        {
            size_t n = 0;
            if (fixed)
            {
                auto acquired = co_await io.acquire_buffer();
                if (!acquired) break;
                BufferLease lease = std::move(*acquired);
                while (n < kEchoMsg)
                {
                    auto r = co_await async_read_fixed(io, fd, lease, kEchoMsg - n, n, ~0ULL);
                    if (!r || *r <= 0)
                    {
                        lease.reset();
                        goto out;
                    }
                    n += static_cast<size_t>(*r);
                }
                auto w = co_await async_write_fixed(io, fd, lease, n, 0, ~0ULL);
                lease.reset();
                if (!w || *w <= 0) break;
            }
            else
            {
                while (n < kEchoMsg)
                {
                    auto r = co_await async_read(io, fd, std::span{heap_buf + n, kEchoMsg - n}, ~0ULL);
                    if (!r || *r <= 0) goto out;
                    n += static_cast<size_t>(*r);
                }
                size_t sent = 0;
                while (sent < n)
                {
                    auto w = co_await async_send(io, fd, std::span{heap_buf + sent, n - sent},
                                                 MSG_NOSIGNAL);
                    if (!w || *w <= 0) goto out;
                    sent += static_cast<size_t>(*w);
                }
            }
        }
    out:
        co_await async_close(io, std::move(fd));
        co_return {};
    }

    Task<> handle_client_http(io::Context& io, io::UniqueFd fd, const bool fixed)
    {
        std::byte heap_buf[4096];
        for (;;)
        {
            if (fixed)
            {
                auto acquired = co_await io.acquire_buffer();
                if (!acquired) break;
                BufferLease lease = std::move(*acquired);
                auto n = co_await async_read_fixed(io, fd, lease, lease.size(), 0, ~0ULL)
                             .timeout(30s);
                if (!n || *n <= 0) break;
                std::memcpy(lease.data(), kHttpResponse.data(), kHttpResponse.size());
                auto w = co_await async_write_fixed(io, fd, lease, kHttpResponse.size(), 0, ~0ULL)
                             .timeout(30s);
                lease.reset();
                if (!w || *w <= 0) break;
            }
            else
            {
                auto n = co_await async_recv(io, fd, std::span{heap_buf}).timeout(30s);
                if (!n || *n <= 0) break;
                auto w = co_await async_send(
                    io, fd,
                    std::span{reinterpret_cast<const std::byte*>(kHttpResponse.data()),
                              kHttpResponse.size()},
                    MSG_NOSIGNAL);
                if (!w || *w <= 0) break;
            }
        }
        co_await async_close(io, std::move(fd));
        co_return {};
    }

    Task<> acceptor(io::Context& io, io::UniqueFd& lfd, const bool fixed, const bool echo)
    {
        for (;;)
        {
            auto c = co_await async_accept(io, lfd);
            if (!c)
            {
                if (c.error().value() == EMFILE || c.error().value() == ENFILE)
                {
                    co_await async_sleep(io, 100ms);
                }
                continue;
            }
            io.spawn(echo ? handle_client(io, io::UniqueFd(*c), fixed)
                          : handle_client_http(io, io::UniqueFd(*c), fixed));
        }
    }

    int make_listener(int port)
    {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || listen(lfd, 512) < 0)
        {
            std::perror("bind/listen");
            std::exit(1);
        }
        return lfd;
    }

    void serve(const unsigned id, const int port, const bool fixed, const bool echo)
    {
        // echo: slot must hold a 64 KiB message; slots sized to outlive the
        // deepest per-thread connection count.
        BufferPoolConfig cfg{.slot_size = echo ? kEchoMsg : 4096, .slots = 512};
        io::Context io(id, nullptr, io::IoOptions{},
                       fixed ? std::optional<BufferPoolConfig>{cfg} : std::nullopt);
        io::UniqueFd fd(make_listener(port));
        io.spawn(acceptor(io, fd, fixed, echo));
        io.run();
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::println(stderr, "usage: {} <normal|fixed> <http|echo> <port> [threads]", argv[0]);
        return 1;
    }
    const bool fixed = std::string_view{argv[1]} == "fixed";
    const bool echo = std::string_view{argv[2]} == "echo";
    const int port = std::atoi(argv[3]);
    const int threads = argc > 4 ? std::atoi(argv[4]) : 4;
    std::println("{} {} listening on 127.0.0.1:{} with {} thread(s)", fixed ? "fixed" : "normal",
                 echo ? "echo" : "http", port, threads);
    std::fflush(stdout);
    std::vector<std::thread> ts;
    for (int i = 1; i < threads; ++i) ts.emplace_back(serve, static_cast<unsigned>(i), port, fixed, echo);
    serve(0, port, fixed, echo);
    for (auto& t : ts) t.join();
}
