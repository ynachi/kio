// UniqueFd ownership + the SockOpt option family.
//
// Closure is observed through fcntl(fd, F_GETFD) == -1 with errno EBADF --
// never by trusting the object's own state. The reset(same_fd) test is the
// regression pin for the review's item 6: re-arming the same fd must swap
// the closer, never close-and-rehold (which would double-close later).

#include "kio/fd.hpp"

#include "kio/net.hpp"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

using kio::io::UniqueFd;
namespace SockOpt = kio::SockOpt;

// ---- compile-time surface ----

static_assert(kio::SocketOption<SockOpt::ReuseAddr>);
static_assert(kio::SocketOption<SockOpt::ReusePort>);
static_assert(kio::SocketOption<SockOpt::NoDelay>);
static_assert(kio::SocketOption<SockOpt::SendBuffer>);
static_assert(kio::SocketOption<SockOpt::RecvBuffer>);
static_assert(kio::SocketOption<SockOpt::KeepAlive>);
static_assert(kio::SocketOption<SockOpt::NonBlocking>);

struct NotAnOption
{
    int dummy;
};
static_assert(!kio::SocketOption<NotAnOption>);

namespace
{
int g_closer_calls = 0;
int g_closer_last_fd = -1;

void counting_closer(const int fd) noexcept
{
    ++g_closer_calls;
    g_closer_last_fd = fd;
    ::close(fd);  // a realistic closer: record AND release
}

int open_dev_null()
{
    return ::open("/dev/null", O_RDWR | O_CLOEXEC);
}

bool fd_is_open(const int fd)
{
    return ::fcntl(fd, F_GETFD) != -1;
}

int tcp_socket()
{
    return ::socket(AF_INET, SOCK_STREAM, 0);
}

int read_int_option(const int fd, const int level, const int name)
{
    int v = -1;
    socklen_t len = sizeof(v);
    EXPECT_EQ(::getsockopt(fd, level, name, &v, &len), 0);
    return v;
}
}  // namespace

// ---- UniqueFd lifetime ----

TEST(UniqueFdTest, DefaultCtorIsInvalid)
{
    const UniqueFd f;
    EXPECT_EQ(f.get(), -1);
    EXPECT_FALSE(f.valid());
}

TEST(UniqueFdTest, DestructorCloses)
{
    int raw = -1;
    {
        UniqueFd f{open_dev_null()};
        raw = f.get();
        ASSERT_GE(raw, 0);
        EXPECT_TRUE(fd_is_open(raw));
    }
    errno = 0;
    EXPECT_FALSE(fd_is_open(raw));
    EXPECT_EQ(errno, EBADF);
}

TEST(UniqueFdTest, MoveCtorEmptiesTheSource)
{
    UniqueFd src{open_dev_null()};
    const int raw = src.get();

    UniqueFd dst{std::move(src)};
    EXPECT_TRUE(dst.valid());
    EXPECT_EQ(dst.get(), raw);
    EXPECT_FALSE(src.valid());  // source dtor must not close anything
    EXPECT_EQ(src.get(), -1);
}

TEST(UniqueFdTest, MoveAssignClosesTheTargetOldFd)
{
    UniqueFd dst{open_dev_null()};
    const int old_raw = dst.get();

    UniqueFd src{open_dev_null()};
    const int new_raw = src.get();

    dst = std::move(src);
    errno = 0;
    EXPECT_FALSE(fd_is_open(old_raw));
    EXPECT_EQ(errno, EBADF);
    EXPECT_TRUE(dst.valid());
    EXPECT_EQ(dst.get(), new_raw);
    EXPECT_FALSE(src.valid());
}

TEST(UniqueFdTest, SelfMoveIsNoop)
{
    UniqueFd f{open_dev_null()};
    const int raw = f.get();

    UniqueFd& alias = f;  // through an alias: self-move is still guarded
    f = std::move(alias);

    EXPECT_TRUE(f.valid());
    EXPECT_EQ(f.get(), raw);
    EXPECT_TRUE(fd_is_open(raw));
}

TEST(UniqueFdTest, ResetClosesByDefault)
{
    UniqueFd f{open_dev_null()};
    const int raw = f.get();
    f.reset();
    EXPECT_FALSE(f.valid());
    EXPECT_FALSE(fd_is_open(raw));
}

TEST(UniqueFdTest, ResetToNewFdClosesTheOldOne)
{
    UniqueFd f{open_dev_null()};
    const int old_raw = f.get();
    const int new_raw = open_dev_null();

    f.reset(new_raw);
    EXPECT_FALSE(fd_is_open(old_raw));
    EXPECT_TRUE(fd_is_open(new_raw));

    f.reset(-1);
    EXPECT_FALSE(fd_is_open(new_raw));
}

// Review item 6 regression: reset() with the SAME fd must not close it --
// close-and-rehold would store a closed fd and double-close it later. It
// swaps the closer, and the destructor runs exactly the installed closer.
TEST(UniqueFdTest, ResetSameFdSwapsCloserWithoutClosing)
{
    g_closer_calls = 0;
    g_closer_last_fd = -1;
    const int raw = open_dev_null();

    {
        UniqueFd f{raw, UniqueFd::default_closer};
        f.reset(raw, counting_closer);
        EXPECT_TRUE(fd_is_open(raw));
        EXPECT_EQ(g_closer_calls, 0);
    }

    EXPECT_EQ(g_closer_calls, 1);
    EXPECT_EQ(g_closer_last_fd, raw);
    EXPECT_FALSE(fd_is_open(raw));
}

TEST(UniqueFdTest, ReleaseReturnsTheFdAndClosesNothing)
{
    const int raw = open_dev_null();
    int got = -1;
    {
        UniqueFd f{raw};
        got = f.release();
        EXPECT_FALSE(f.valid());
    }  // dtor: fd_ == -1, nothing to close

    EXPECT_EQ(got, raw);
    EXPECT_TRUE(fd_is_open(raw));
    EXPECT_EQ(::close(raw), 0);
}

TEST(UniqueFdTest, NoopCloserLeavesTheFdOpen)
{
    const int raw = open_dev_null();
    {
        const UniqueFd f{raw, UniqueFd::noop_closer};
    }
    EXPECT_TRUE(fd_is_open(raw));
    EXPECT_EQ(::close(raw), 0);
}

TEST(UniqueFdTest, CountingCloserRunsExactlyOnceWithTheRightFd)
{
    g_closer_calls = 0;
    g_closer_last_fd = -1;
    const int raw = open_dev_null();
    {
        const UniqueFd f{raw, counting_closer};
    }
    EXPECT_EQ(g_closer_calls, 1);
    EXPECT_EQ(g_closer_last_fd, raw);
}

// ---- SockOpt ----

TEST(SockOptTest, BooleanOptionsReadBackFromATcpSocket)
{
    const UniqueFd sock{tcp_socket()};
    ASSERT_GE(sock.get(), 0);

    EXPECT_TRUE(sock.set_option(SockOpt::ReuseAddr{}).has_value());
    EXPECT_EQ(read_int_option(sock.get(), SOL_SOCKET, SO_REUSEADDR), 1);

    EXPECT_TRUE(sock.set_option(SockOpt::ReusePort{}).has_value());
    EXPECT_EQ(read_int_option(sock.get(), SOL_SOCKET, SO_REUSEPORT), 1);

    EXPECT_TRUE(sock.set_option(SockOpt::KeepAlive{}).has_value());
    EXPECT_EQ(read_int_option(sock.get(), SOL_SOCKET, SO_KEEPALIVE), 1);

    EXPECT_TRUE(sock.set_option(SockOpt::NoDelay{}).has_value());
    EXPECT_EQ(read_int_option(sock.get(), IPPROTO_TCP, TCP_NODELAY), 1);

    // Disabling writes 0 back.
    EXPECT_TRUE(sock.set_option(SockOpt::NoDelay{false}).has_value());
    EXPECT_EQ(read_int_option(sock.get(), IPPROTO_TCP, TCP_NODELAY), 0);
}

// Linux doubles buffer requests and enforces a floor, so only a lower bound
// is assertable.
TEST(SockOptTest, BufferOptionsAreAtLeastTheRequestedSize)
{
    const UniqueFd sock{tcp_socket()};
    ASSERT_GE(sock.get(), 0);

    constexpr int kWant = 256 * 1024;
    EXPECT_TRUE(sock.set_option(SockOpt::SendBuffer{kWant}).has_value());
    EXPECT_GE(read_int_option(sock.get(), SOL_SOCKET, SO_SNDBUF), kWant);

    EXPECT_TRUE(sock.set_option(SockOpt::RecvBuffer{kWant}).has_value());
    EXPECT_GE(read_int_option(sock.get(), SOL_SOCKET, SO_RCVBUF), kWant);
}

TEST(SockOptTest, NonBlockingTogglesTheFlag)
{
    const UniqueFd sock{tcp_socket()};
    ASSERT_GE(sock.get(), 0);

    const int before = ::fcntl(sock.get(), F_GETFL);
    ASSERT_NE(before, -1);
    EXPECT_EQ(before & O_NONBLOCK, 0);

    EXPECT_TRUE(sock.set_option(SockOpt::NonBlocking{}).has_value());
    EXPECT_NE(::fcntl(sock.get(), F_GETFL) & O_NONBLOCK, 0);

    EXPECT_TRUE(sock.set_option(SockOpt::NonBlocking{false}).has_value());
    EXPECT_EQ(::fcntl(sock.get(), F_GETFL) & O_NONBLOCK, 0);
}

// The fold short-circuits on the first error: a failing NoDelay on an
// AF_UNIX socket must leave the later KeepAlive unattempted (SO_KEEPALIVE
// still 0), with the error naming the failed option.
TEST(SockOptTest, SetOptionsShortCircuitsOnFirstError)
{
    int sv[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    const UniqueFd a{sv[0]};

    const auto r = a.set_options(SockOpt::NoDelay{}, SockOpt::KeepAlive{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().value(), EOPNOTSUPP);
    EXPECT_EQ(r.error().context(), "setsockopt");

    EXPECT_EQ(read_int_option(sv[0], SOL_SOCKET, SO_KEEPALIVE), 0);

    EXPECT_EQ(::close(sv[1]), 0);
}

TEST(SockOptTest, SetOptionsAppliesEverythingOnSuccess)
{
    const UniqueFd sock{tcp_socket()};
    ASSERT_GE(sock.get(), 0);

    const auto r = sock.set_options(SockOpt::ReuseAddr{}, SockOpt::KeepAlive{}, SockOpt::NoDelay{});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(read_int_option(sock.get(), SOL_SOCKET, SO_REUSEADDR), 1);
    EXPECT_EQ(read_int_option(sock.get(), SOL_SOCKET, SO_KEEPALIVE), 1);
    EXPECT_EQ(read_int_option(sock.get(), IPPROTO_TCP, TCP_NODELAY), 1);
}

TEST(SockOptTest, EmptyOptionPackSucceeds)
{
    const UniqueFd sock{tcp_socket()};
    ASSERT_GE(sock.get(), 0);
    EXPECT_TRUE(sock.set_options().has_value());
}

TEST(SockOptTest, BadFdReportsErrnoFromTheOption)
{
    int raw = open_dev_null();
    ASSERT_GE(raw, 0);
    ASSERT_EQ(::close(raw), 0);

    const UniqueFd f{raw};  // wraps an already-closed fd
    const auto sock_err = f.set_option(SockOpt::ReuseAddr{});
    ASSERT_FALSE(sock_err.has_value());
    EXPECT_EQ(sock_err.error().value(), EBADF);
    EXPECT_EQ(sock_err.error().context(), "setsockopt");

    const auto fcntl_err = f.set_option(SockOpt::NonBlocking{});
    ASSERT_FALSE(fcntl_err.has_value());
    EXPECT_EQ(fcntl_err.error().value(), EBADF);
    EXPECT_EQ(fcntl_err.error().context(), "fcntl");

    const auto pack_err = f.set_options(SockOpt::ReuseAddr{}, SockOpt::KeepAlive{});
    ASSERT_FALSE(pack_err.has_value());
    EXPECT_EQ(pack_err.error().value(), EBADF);
}
