// End-to-end: one echo server ties the whole stack together -- TcpListener
// bind, an accept loop spawning handler tasks, a Semaphore limiting handler
// concurrency, linked timeouts on every server I/O, 50 concurrent clients
// each moving 1 MiB in lock-step ping-pong chunks, and a monitor task that
// fires cancel_all() once every client is done so run() RETURNS (the v2
// teardown story). Afterwards every fd must be back in the bookkeeping.
//
// Clients are plain blocking threads on the other end of real sockets:
// lock-step (send a chunk, read it back) avoids the both-sides-blocked
// deadlock full-duplex bulk transfer would hit with >socket-buffer volume.

#include "kio/context.h"
#include "kio/operations.h"
#include "kio/semaphore.h"
#include "kio/task.h"

#include "kio/net.hpp"
#include "kio/tcp_listener.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>

#include "test_util.hpp"
#include <gtest/gtest.h>
#include <netinet/in.h>

using namespace kio;
using namespace std::chrono_literals;

namespace
{
constexpr int kClients = 50;
constexpr int kRounds = 256;  // 256 x 4 KiB = 1 MiB per client
constexpr int kChunk = 4096;
constexpr auto kIoTimeout = 5s;

Result<io::UniqueFd> bind_local_listener(SocketAddress& out)
{
    auto l = TcpListener::Bind(0, "127.0.0.1");
    if (!l)
    {
        return l;
    }
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(l->get(), reinterpret_cast<sockaddr*>(&ss), &len) != 0)
    {
        return std::unexpected(Error::fail_errno(errno, "getsockname"));
    }
    SocketAddress a{};
    a.addr = ss;
    a.addrlen = len;
    out = a;
    return l;
}

std::byte pattern_byte(const int client, const int round, const int off)
{
    return static_cast<std::byte>((client * 131 + round * 17 + off) & 0xff);
}

// Handler: echoes until the peer closes, under the concurrency semaphore,
// every I/O armed with a linked timeout. A wedged op surfaces as
// ETIMEDOUT instead of hanging the whole run.
Task<> echo_handler(io::Context& io, Semaphore& permits, const int conn_fd, std::atomic<int>& io_failures)
{
    const io::UniqueFd conn{conn_fd};  // take ownership of the accepted fd
    co_await permits.acquire();

    std::array<std::byte, kChunk> buf{};
    for (;;)
    {
        auto rop = async_recv(io, conn, std::span{buf});
        rop.timeout(kIoTimeout);
        auto r = co_await rop;
        if (!r.has_value())
        {
            if (r.error().value() != ECANCELED)
            {
                ++io_failures;
            }
            break;
        }
        if (*r == 0)
        {
            break;  // peer closed: clean end
        }

        auto wop = async_send(io, conn, std::span{buf}.first(static_cast<size_t>(*r)));
        wop.timeout(kIoTimeout);
        auto w = co_await wop;
        if (!w.has_value() || *w != *r)
        {
            ++io_failures;
            break;
        }
    }

    permits.release();
    co_return {};
}

// Accept loop: every connection becomes a spawned handler task. Exits when
// teardown's cancel_all() cancels the pending accept (ECANCELED).
Task<> accept_loop(io::Context& io, Semaphore& permits, const io::UniqueFd& listener, std::atomic<int>& io_failures)
{
    for (;;)
    {
        auto conn = co_await async_accept(io, listener);
        if (!conn.has_value())
        {
            if (conn.error().value() != ECANCELED)
            {
                ++io_failures;
            }
            co_return {};
        }
        io.spawn(echo_handler(io, permits, *conn, io_failures));
    }
}

// Polls until every client reported done, then tears the loop down: the
// pending accept is canceled and run() returns.
Task<> completion_monitor(io::Context& io, const std::atomic<int>& done)
{
    for (;;)
    {
        if (done.load() >= kClients)
        {
            io.cancel_all();
            co_return {};
        }
        auto z = co_await async_sleep(io, 2ms);
        (void)z;
    }
}

void client_thread(const int id, const SocketAddress& addr, std::atomic<int>& done, std::vector<char>& verified,
                   std::atomic<int>& connect_failures)
{
    int fd = -1;
    for (int attempt = 0; attempt < 100 && fd == -1; ++attempt)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1)
        {
            break;
        }
        if (::connect(fd, addr.Get(), addr.addrlen) != 0)
        {
            ::close(fd);
            fd = -1;
            std::this_thread::sleep_for(10ms);
        }
    }
    if (fd == -1)
    {
        ++connect_failures;
        done.fetch_add(1);
        return;
    }

    bool ok = true;
    std::array<std::byte, kChunk> send_buf{};
    std::array<std::byte, kChunk> recv_buf{};
    for (int round = 0; round < kRounds && ok; ++round)
    {
        for (int i = 0; i < kChunk; ++i)
        {
            send_buf[static_cast<size_t>(i)] = pattern_byte(id, round, i);
        }
        if (::send(fd, send_buf.data(), kChunk, MSG_NOSIGNAL) != kChunk)
        {
            ok = false;
            break;
        }

        size_t got = 0;
        while (got < kChunk && ok)
        {
            const ssize_t n = ::recv(fd, recv_buf.data() + got, kChunk - got, 0);
            if (n <= 0)
            {
                ok = false;
                break;
            }
            got += static_cast<size_t>(n);
        }
        for (int i = 0; i < kChunk && ok; ++i)
        {
            if (recv_buf[static_cast<size_t>(i)] != pattern_byte(id, round, i))
            {
                ok = false;
            }
        }
    }

    verified[static_cast<size_t>(id)] = ok ? 1 : 0;
    ::close(fd);
    done.fetch_add(1);
}
}  // namespace

TEST(EchoTest, FiftyConcurrentClientsMoveOneMiBEach)
{
    SocketAddress laddr{};
    auto listener = bind_local_listener(laddr);
    ASSERT_TRUE(listener.has_value());

    io::IoOptions opts;
    opts.entries = 256;
    opts.batch_max_size = 32;
    io::Context ctx(0, nullptr, opts);
    Semaphore permits(ctx, 16);

    // Baseline AFTER the ring exists: the leak check then covers every
    // per-connection fd without counting the ring's own descriptors.
    const auto fd_baseline = test::OpenFdCount();

    std::atomic<int> done{0};
    std::atomic<int> io_failures{0};
    std::atomic<int> connect_failures{0};
    std::vector<char> verified(kClients, 0);  // char, not vector<bool>: no shared-word races

    ctx.spawn(accept_loop(ctx, permits, *listener, io_failures));
    ctx.spawn(completion_monitor(ctx, done));

    // Clients start BEFORE run(): their connects complete through the
    // listen backlog and their first send fits the socket buffer, so they
    // park in recv until the loop starts echoing. run() returns when the
    // monitor fires cancel_all().
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int i = 0; i < kClients; ++i)
    {
        clients.emplace_back(client_thread, i, laddr, std::ref(done), std::ref(verified), std::ref(connect_failures));
    }

    ctx.run();

    for (auto& t : clients)
    {
        t.join();
    }

    EXPECT_EQ(connect_failures.load(), 0);
    EXPECT_EQ(io_failures.load(), 0);
    for (int i = 0; i < kClients; ++i)
    {
        EXPECT_TRUE(verified[static_cast<size_t>(i)]) << "client " << i << " checksum mismatch";
    }
    EXPECT_EQ(done.load(), kClients);

    // Teardown must leave the fd table clean: server handlers closed their
    // accepted sockets, the accept loop unwound, clients closed theirs.
    // The listener fd is still held here -- it is part of the baseline too.
    ctx.run();  // idle session: proves quiescence after teardown
    EXPECT_EQ(test::OpenFdCount(), fd_baseline);
    listener->reset();
}
