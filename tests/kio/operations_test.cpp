// The synchronous-ish operation surface: read/write/readv/writev, send/recv,
// accept/connect, open/close/remove/rename, fsync/ftruncate/fallocate, poll,
// and sleep -- everything except fixed-buffer ops (pinned in
// buffer_pool_test.cpp) and timeout/cancel classification
// (timing_cancel_test.cpp).
//
// Real fds everywhere: pipes, socketpairs, and files under a per-test
// mkdtemp directory. Every fd owned by a test outlives the ctx.run() that
// uses it -- an awaitable only captures the raw fd number. EXPECT (not
// ASSERT) inside coroutine bodies.

#include "kio/operations.h"

#include "kio/context.h"
#include "kio/task.h"

#include "kio/error.hpp"
#include "kio/net.hpp"
#include "kio/tcp_listener.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "test_util.hpp"
#include <gtest/gtest.h>

using namespace kio;
using namespace std::chrono_literals;

namespace
{
    // ---- driving ----

    template <typename F>
    Task<> op_task(io::Context& io, F make, Result<int>& out)
    {
        out = co_await make(io);
        co_return {};
    }

    // Runs one awaitable-producing lambda to completion on a fresh Context.
    // Lambdas capture by reference: the captured objects must outlive run(),
    // which this helper guarantees by scope.
    template <typename F>
    Result<int> run_io_op(F&& make)
    {
        io::Context ctx(0, nullptr, test::test_opts());
        Result<int> out{std::unexpect, Error::from_errc(std::errc::operation_canceled)};
        ctx.spawn(op_task(ctx, std::forward<F>(make), out));
        ctx.run();
        return out;
    }

    io::UniqueFd make_temp_file(const std::filesystem::path& dir, const char* name, const std::string& payload = "")
    {
        const auto path = (dir / name).string();
        io::UniqueFd fd{::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600)};
        if (!payload.empty() && fd.valid())
        {
            (void)::write(fd.get(), payload.data(), payload.size());
            (void)::lseek(fd.get(), 0, SEEK_SET);
        }
        return fd;
    }

    // A loopback listener plus its resolved address (port filled in).
    struct LocalListener
    {
        io::UniqueFd fd;
        SocketAddress addr{};
    };

    Result<LocalListener> bind_local_listener()
    {
        auto l = TcpListener::bind(0, "127.0.0.1");
        if (!l)
        {
            return std::unexpected(l.error());
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
        return LocalListener{std::move(*l), a};
    }

    int connect_blocking(const SocketAddress& addr)
    {
        const int c = ::socket(AF_INET, SOCK_STREAM, 0);
        if (c == -1)
        {
            return -1;
        }
        if (::connect(c, addr.Get(), addr.addrlen) != 0)
        {
            ::close(c);
            return -1;
        }
        return c;
    }

    // clamp_len is a pure constexpr function: pin it at compile time.
    static_assert(detail::clamp_len(0x7ffff000u) == 0x7ffff000u);
    static_assert(detail::clamp_len(0x7ffff001u) == 0x7ffff000u);
    static_assert(detail::clamp_len((size_t{1} << 32) + 10) == 0x7ffff000u);
    static_assert(detail::clamp_len(0) == 0);
} // namespace

// ---- read / write ----

TEST(OperationsTest, PipeWriteThenReadRoundTrips)
{
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const io::UniqueFd r{fds[0]};
    const io::UniqueFd w{fds[1]};

    io::Context ctx(0, nullptr, test::test_opts());
    Result<int> wout{0};
    Result<int> rout{0};
    bool match = false;

    auto task = [](io::Context& io, const io::UniqueFd& rfd, const io::UniqueFd& wfd, Result<int>& wout,
                   Result<int>& rout, bool& match) -> Task<>
    {
        const std::byte send[] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        wout = co_await async_write(io, wfd, std::span{send, 4});
        std::byte recv[4]{};
        rout = co_await async_read(io, rfd, std::span{recv, 4});
        match = std::memcmp(recv, send, 4) == 0;
        co_return {};
    };
    ctx.spawn(task(ctx, r, w, wout, rout, match));
    ctx.run();

    ASSERT_TRUE(wout.has_value());
    EXPECT_EQ(*wout, 4);
    ASSERT_TRUE(rout.has_value());
    EXPECT_EQ(*rout, 4);
    EXPECT_TRUE(match);
}

// off == ~0ULL means "current file position, then advance": sequential
// reads walk the file, and an explicit offset reads positionally.
TEST(OperationsTest, ReadAdvancesPositionAndExplicitOffsetIsPositional)
{
    test::TempDir dir;
    io::UniqueFd fd = make_temp_file(dir.path(), "positional.bin", "hello world");

    auto res = run_io_op(
        [&](io::Context& io)
        {
            std::byte buf[16]{};
            return async_read(io, fd, std::span{buf, 5}); // pos 0 -> 5
        });
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 5);

    res = run_io_op(
        [&](io::Context& io)
        {
            std::byte buf[16]{};
            return async_read(io, fd, std::span{buf, 5}); // pos 5 -> 10
        });
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 5);

    // Explicit offset (pread-style): reads at 0 regardless of the position.
    res = run_io_op(
        [&](io::Context& io)
        {
            std::byte buf[16]{};
            return async_read(io, fd, std::span{buf, 11}, 0);
        });
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 11);
}

TEST(OperationsTest, ZeroLengthSpanCompletesWithZero)
{
    test::TempDir dir;
    const io::UniqueFd fd = make_temp_file(dir.path(), "empty.bin", "data");

    const auto res = run_io_op(
        [&](io::Context& io)
        {
            std::byte b{};
            return async_read(io, fd, std::span{&b, 0});
        });
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 0);
}

// ---- readv / writev ----

TEST(OperationsTest, WritevThenReadvScatterGather)
{
    test::TempDir dir;
    const io::UniqueFd fd = make_temp_file(dir.path(), "vector.bin");

    // The iovec array must stay alive until the operation completes: the
    // awaitable captures only a span. Everything lives in THIS scope,
    // which outlasts both run() calls.
    std::byte a[3] = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    std::byte b[2] = {std::byte{'d'}, std::byte{'e'}};
    std::byte c[4] = {std::byte{'f'}, std::byte{'g'}, std::byte{'h'}, std::byte{'i'}};
    const std::vector<iovec> wiov{
        {a, 3},
        {b, 2},
        {c, 4}
    };

    const auto w = run_io_op([&](io::Context& io) { return async_writev(io, fd, wiov, 0); });
    ASSERT_TRUE(w.has_value()) << w.error().message();
    EXPECT_EQ(*w, 9);

    std::byte x[4]{};
    std::byte y[5]{};
    const std::vector<iovec> riov{
        {x, 4},
        {y, 5}
    };

    const auto r = run_io_op([&](io::Context& io) { return async_readv(io, fd, riov, 0); });
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(*r, 9);

    char got[9]{};
    ASSERT_EQ(::pread(fd.get(), got, 9, 0), 9);
    EXPECT_EQ((std::string_view{got, 9}), "abcdefghi"); // parens: the braces' comma would split the macro args
    EXPECT_EQ(std::memcmp(x, "abcd", 4), 0);
    EXPECT_EQ(std::memcmp(y, "efghi", 5), 0);
}

// ---- send / recv ----

TEST(OperationsTest, SocketpairEcho)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    const io::UniqueFd a{sv[0]};
    const io::UniqueFd b{sv[1]};

    io::Context ctx(0, nullptr, test::test_opts());
    Result<int> sent{0};
    Result<int> got{0};
    bool match = false;

    auto task = [](io::Context& io, const io::UniqueFd& left, const io::UniqueFd& right, Result<int>& sent,
                   Result<int>& got, bool& match) -> Task<>
    {
        const std::byte msg[] = {std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
        std::byte echo[4]{};
        sent = co_await async_send(io, left, std::span{msg, 4});
        got = co_await async_recv(io, right, std::span{echo, 4});
        match = std::memcmp(echo, msg, 4) == 0;
        co_return {};
    };
    ctx.spawn(task(ctx, a, b, sent, got, match));
    ctx.run();

    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(*sent, 4);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 4);
    EXPECT_TRUE(match);
}

// Review item 2 regression: async_send defaults to MSG_NOSIGNAL, so a dead
// peer produces an EPIPE completion -- no signal, process survives.
TEST(OperationsTest, SendToDeadPeerGivesEpipeWithoutSigpipe)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    const io::UniqueFd a{sv[0]};
    ::close(sv[1]); // the peer dies before the send

    static sig_atomic_t g_sigpipe_caught = 0;
    struct sigaction sa{};
    sa.sa_handler = +[](int) { ++g_sigpipe_caught; };
    struct sigaction old{};
    ASSERT_EQ(::sigaction(SIGPIPE, &sa, &old), 0);
    g_sigpipe_caught = 0;

    const auto res = run_io_op(
        [&](io::Context& io)
        {
            const std::byte b = std::byte{'x'};
            return async_send(io, a, std::span{&b, 1});
        });

    ::sigaction(SIGPIPE, &old, nullptr);

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EPIPE);
    EXPECT_EQ(res.error().context(), "io operation failed");
    EXPECT_EQ(g_sigpipe_caught, 0) << "MSG_NOSIGNAL default must suppress SIGPIPE";
}

// The async_write warning is real: on a socket it has write(2) semantics.
// With a handler installed the signal is caught and the write fails EPIPE --
// pinning that the process survives AND that the failure is EPIPE.
TEST(OperationsTest, AsyncWriteToDeadPeerRaisesCaughtSigpipeAndEpipe)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    const io::UniqueFd a{sv[0]};
    ::close(sv[1]);

    static sig_atomic_t g_sigpipe_caught = 0;
    struct sigaction sa{};
    sa.sa_handler = +[](int) { ++g_sigpipe_caught; };
    struct sigaction old{};
    ASSERT_EQ(::sigaction(SIGPIPE, &sa, &old), 0);
    g_sigpipe_caught = 0;

    const auto res = run_io_op(
        [&](io::Context& io)
        {
            const std::byte b = std::byte{'x'};
            return async_write(io, a, std::span{&b, 1});
        });

    ::sigaction(SIGPIPE, &old, nullptr);

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().value(), EPIPE);
    EXPECT_EQ(g_sigpipe_caught, 1) << "async_write on a socket has write(2) SIGPIPE semantics";
}

// ---- accept / connect ----

TEST(OperationsTest, AcceptReturnsNonblockCloexecSocket)
{
    auto listener = bind_local_listener();
    ASSERT_TRUE(listener.has_value());

    const int client = connect_blocking(listener->addr);
    ASSERT_GE(client, 0);
    ASSERT_EQ(::write(client, "z", 1), 1);

    io::Context ctx(0, nullptr, test::test_opts());
    int accepted_flags = -1;
    int accepted_fdflags = -1;
    Result<int> first_byte{0};

    auto task = [](io::Context& io, const io::UniqueFd& lfd, int* out_flags, int* out_fdflags,
                   Result<int>& first_byte) -> Task<>
    {
        // async_accept's CQE is the raw accepted fd; the UniqueFd here takes
        // ownership so it is closed when the handler ends.
        auto conn = co_await async_accept(io, lfd);
        if (!conn)
        {
            first_byte = std::unexpected(conn.error());
            co_return std::unexpected(conn.error());
        }
        io::UniqueFd acc{*conn};
        *out_flags = ::fcntl(acc.get(), F_GETFL);
        *out_fdflags = ::fcntl(acc.get(), F_GETFD);
        std::byte b{};
        first_byte = co_await async_recv(io, acc, std::span{&b, 1});
        co_return {};
    };
    ctx.spawn(task(ctx, listener->fd, &accepted_flags, &accepted_fdflags, first_byte));
    ctx.run();

    EXPECT_NE(accepted_flags & O_NONBLOCK, 0) << "accepted socket must be non-blocking";
    EXPECT_NE(accepted_fdflags & FD_CLOEXEC, 0) << "accepted socket must be close-on-exec";

    ASSERT_TRUE(first_byte.has_value());
    EXPECT_EQ(*first_byte, 1);
    EXPECT_EQ(::close(client), 0);
}

TEST(OperationsTest, AcceptOverloadFillsClientAddressAndResetsAddrlen)
{
    auto listener = bind_local_listener();
    ASSERT_TRUE(listener.has_value());

    const int client = connect_blocking(listener->addr);
    ASSERT_GE(client, 0);

    sockaddr_storage client_ss{};
    socklen_t client_len = sizeof(client_ss);
    ASSERT_EQ(::getsockname(client, reinterpret_cast<sockaddr*>(&client_ss), &client_len), 0);
    const auto* cin = reinterpret_cast<const sockaddr_in*>(&client_ss);
    const uint16_t client_port = ntohs(cin->sin_port);

    io::Context ctx(0, nullptr, test::test_opts());
    SocketAddress client_addr{}; // must outlive the operation: captured by reference
    client_addr.addrlen = 3; // garbage: the op resets it per attempt
    bool addr_ok = false;
    bool accepted = false;

    auto task = [](io::Context& io, const io::UniqueFd& lfd, SocketAddress& client_addr, bool& addr_ok, bool& accepted,
                   const uint16_t want_port) -> Task<>
    {
        auto conn = co_await async_accept(io, lfd, client_addr);
        if (!conn)
        {
            co_return std::unexpected(conn.error());
        }
        accepted = true;
        addr_ok = client_addr.GetIp().value_or("?") == "127.0.0.1" && client_addr.GetPort() == want_port;
        co_return {};
    };
    ctx.spawn(task(ctx, listener->fd, client_addr, addr_ok, accepted, client_port));
    ctx.run();

    EXPECT_TRUE(accepted);
    EXPECT_TRUE(addr_ok);
    EXPECT_EQ(client_addr.addrlen, sizeof(sockaddr_in));
    EXPECT_EQ(::close(client), 0);
}

TEST(OperationsTest, ConnectSucceedsToAListener)
{
    auto listener = bind_local_listener();
    ASSERT_TRUE(listener.has_value());

    // The client fd lives in the TEST scope: an awaitable captures only the
    // raw number, so the fd must stay open past run().
    io::UniqueFd c1{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    auto r1 =
        run_io_op([&](io::Context& io) { return async_connect(io, c1, listener->addr.Get(), listener->addr.addrlen); });
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    io::UniqueFd c2{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    auto r2 = run_io_op([&](io::Context& io) { return async_connect(io, c2, listener->addr); });
    ASSERT_TRUE(r2.has_value()) << r2.error().message();
}

TEST(OperationsTest, ConnectToAClosedPortGivesEconnrefused)
{
    auto closed_addr = SocketAddress::V4(1, "127.0.0.1"); // nothing listens on 1
    ASSERT_TRUE(closed_addr.has_value());
    io::UniqueFd c{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};

    const auto r = run_io_op([&](io::Context& io) { return async_connect(io, c, *closed_addr); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().value(), ECONNREFUSED);
    EXPECT_EQ(r.error().context(), "io operation failed");
}

// ---- open / close / remove / rename ----

TEST(OperationsTest, OpenMissingFileGivesEnoent)
{
    test::TempDir dir;
    const auto missing = (dir.path() / "does-not-exist.bin").string();

    const auto r = run_io_op(
        [&](io::Context& io)
        {
            // The path is moved into the awaiter's lambda, which outlives
            // the call expression -- passing the path as a temporary is the
            // pattern ASan guards.
            return async_open(io, std::filesystem::path{missing}, O_RDONLY);
        });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().value(), ENOENT);
    EXPECT_EQ(r.error().context(), "io operation failed");
}

TEST(OperationsTest, OpenWithCreateHonorsTheMode)
{
    test::TempDir dir;
    const auto target = (dir.path() / "created-file-with-a-long-name.bin").string();

    const auto r =
        run_io_op([&](io::Context& io)
        {
            return async_open(io, std::filesystem::path{target}, O_CREAT | O_WRONLY | O_CLOEXEC, 0640);
        });
    ASSERT_TRUE(r.has_value());

    struct stat st{};
    ASSERT_EQ(::stat(target.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0640);
}

TEST(OperationsTest, RenameMovesAndRemoveDeletes)
{
    test::TempDir dir;
    const auto from = (dir.path() / "rename-source-with-a-long-name.bin").string();
    const auto to = (dir.path() / "rename-target-with-a-longer-name.bin").string();
    ASSERT_EQ(::close(::open(from.c_str(), O_CREAT | O_RDWR, 0600)), 0);

    auto r = run_io_op([&](io::Context& io)
    {
        return async_rename(io, std::filesystem::path{from}, std::filesystem::path{to});
    });
    ASSERT_TRUE(r.has_value()) << r.error().message();

    struct stat st{};
    EXPECT_EQ(::stat(from.c_str(), &st), -1); // source gone
    EXPECT_EQ(::stat(to.c_str(), &st), 0); // target exists

    r = run_io_op([&](io::Context& io) { return async_remove(io, std::filesystem::path{to}); });
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(::stat(to.c_str(), &st), -1);
}

TEST(OperationsTest, AsyncCloseClosesExactlyOnce)
{
    test::TempDir dir;
    io::UniqueFd fd = make_temp_file(dir.path(), "closable.bin");
    const int raw = fd.get();

    const auto r = run_io_op(
        [&](io::Context& io)
        {
            // Consumes the UniqueFd (the fd is released at submit time);
            // the moved-from shell in the test scope closes nothing again.
            return async_close(io, std::move(fd));
        });
    ASSERT_TRUE(r.has_value());

    errno = 0;
    EXPECT_EQ(::fcntl(raw, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

// ---- fsync / fallocate / ftruncate ----

TEST(OperationsTest, FsyncFallocateFtruncateResizeTheFile)
{
    test::TempDir dir;
    const io::UniqueFd fd = make_temp_file(dir.path(), "sized.bin");

    // fallocate: some filesystems refuse; skip-on-EINVAL/EOPNOTSUPP per the
    // review's instruction.
    if (auto fal = run_io_op([&](io::Context& io) { return async_fallocate(io, fd, 0, 0, 8192); }); fal.has_value())
    {
        struct stat st{};
        ASSERT_EQ(::fstat(fd.get(), &st), 0);
        EXPECT_EQ(st.st_size, 8192);
    }
    else
    {
        EXPECT_TRUE(fal.error().value() == EINVAL || fal.error().value() == EOPNOTSUPP) << fal.error().message();
    }

    auto trunc = run_io_op([&](io::Context& io) { return async_ftruncate(io, fd, 4096); });
    ASSERT_TRUE(trunc.has_value()) << trunc.error().message();
    struct stat st{};
    ASSERT_EQ(::fstat(fd.get(), &st), 0);
    EXPECT_EQ(st.st_size, 4096);

    for (const bool full : {false, true})
    {
        auto fs = run_io_op([&](io::Context& io) { return async_fsync(io, fd, full); });
        ASSERT_TRUE(fs.has_value()) << fs.error().message();
    }
}

// ---- poll ----

TEST(OperationsTest, PollReportsReadableAndWritableMasks)
{
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const io::UniqueFd r{fds[0]};
    const io::UniqueFd w{fds[1]};
    ASSERT_EQ(::write(fds[1], "q", 1), 1); // readable immediately

    auto readable = run_io_op([&](io::Context& io) { return async_poll(io, r, POLLIN); });
    ASSERT_TRUE(readable.has_value());
    EXPECT_NE(*readable & POLLIN, 0) << "mask: " << *readable;

    auto writable = run_io_op([&](io::Context& io) { return async_poll(io, w, POLLOUT); });
    ASSERT_TRUE(writable.has_value());
    EXPECT_NE(*writable & POLLOUT, 0) << "mask: " << *writable;
}

// ---- sleep ----

TEST(OperationsTest, SleepTenMsReturnsZeroAfterAtLeastTenMs)
{
    const auto start = std::chrono::steady_clock::now();
    const auto res = run_io_op([&](io::Context& io) { return async_sleep(io, 10ms); });
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 0);
    EXPECT_GE(elapsed, 10ms) << "lower bound only: timers may fire late";
}

TEST(OperationsTest, ZeroSleepCompletesImmediately)
{
    const auto res = run_io_op([&](io::Context& io) { return async_sleep(io, 0ms); });
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 0);
}
