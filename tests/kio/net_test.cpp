// SocketAddress / ResolveIp / TcpListener -- the hermetic net layer.
//
// Everything here is synchronous and kernel-adjacent (no ring): literal
// parsing, the any-address fallback, and TcpListener's bind/listen defaults.
// The one contract worth pinning verbatim is tcp_listener.cc's comment:
// TCP_NODELAY set on the listening socket is inherited by accepted clients.

#include "kio/net.hpp"

#include "kio/tcp_listener.hpp"

#include <cerrno>
#include <cstdint>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>

#include "test_util.hpp"
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

using namespace kio;

namespace
{
std::string read_ip(const int fd)
{
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len), 0);
    char buf[INET6_ADDRSTRLEN] = {};
    const auto* sa = reinterpret_cast<const sockaddr*>(&ss);
    const void* src = sa->sa_family == AF_INET
                          ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(sa)->sin_addr)
                          : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr);
    EXPECT_NE(::inet_ntop(sa->sa_family, src, buf, sizeof(buf)), nullptr);
    return buf;
}

uint16_t read_bound_port(const int fd)
{
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len), 0);
    if (ss.ss_family == AF_INET)
    {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port);
}

bool ipv6_available()
{
    const int s = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (s == -1)
    {
        return false;
    }
    ::close(s);
    return true;
}
}  // namespace

// ---- SocketAddress ----

TEST(SocketAddressTest, V4PortAndIpRoundTrip)
{
    const auto r = SocketAddress::V4(8080, "127.0.0.1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->GetPort(), 8080u);
    ASSERT_TRUE(r->GetIp().has_value());
    EXPECT_EQ(*r->GetIp(), "127.0.0.1");
    EXPECT_EQ(r->addrlen, sizeof(sockaddr_in));
}

TEST(SocketAddressTest, V6PortAndIpRoundTrip)
{
    const auto r = SocketAddress::V6(9999, "::1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->GetPort(), 9999u);
    ASSERT_TRUE(r->GetIp().has_value());
    EXPECT_EQ(*r->GetIp(), "::1");
    EXPECT_EQ(r->addrlen, sizeof(sockaddr_in6));
}

TEST(SocketAddressTest, NullAndEmptyIpGiveTheAnyAddress)
{
    auto any = SocketAddress::V4(1234);
    ASSERT_TRUE(any.has_value());
    EXPECT_EQ(any->GetIp().value_or("?"), "0.0.0.0");

    auto empty = SocketAddress::V4(1234, "");
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->GetIp().value_or("?"), "0.0.0.0");

    auto any6 = SocketAddress::V6(1234);
    ASSERT_TRUE(any6.has_value());
    EXPECT_EQ(any6->GetIp().value_or("?"), "::");

    auto empty6 = SocketAddress::V6(1234, "");
    ASSERT_TRUE(empty6.has_value());
    EXPECT_EQ(empty6->GetIp().value_or("?"), "::");
}

TEST(SocketAddressTest, V6AcceptsV4MappedLiteral)
{
    const auto r = SocketAddress::V6(80, "::ffff:1.2.3.4");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->GetIp().value_or("?"), "::ffff:1.2.3.4");
}

TEST(SocketAddressTest, V4RejectsMalformedLiterals)
{
    for (const char* bad : {"999.1.1.1", "localhost", "::1"})
    {
        auto r = SocketAddress::V4(80, bad);
        ASSERT_FALSE(r.has_value()) << bad;
        EXPECT_EQ(r.error().value(), EINVAL) << bad;
        EXPECT_EQ(r.error().context(), "inet_pton(AF_INET)") << bad;
    }
}

// inet_pton has no scope-id support: a scoped literal must be rejected, not
// silently parsed with the zone dropped.
TEST(SocketAddressTest, V6RejectsScopedLiteral)
{
    auto r = SocketAddress::V6(80, "fe80::1%lo");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().value(), EINVAL);
    EXPECT_EQ(r.error().context(), "inet_pton(AF_INET6)");
}

TEST(SocketAddressTest, PortZeroAndPortMaxAreAccepted)
{
    EXPECT_TRUE(SocketAddress::V4(0, "127.0.0.1").has_value());
    EXPECT_TRUE(SocketAddress::V4(65535, "127.0.0.1").has_value());
    EXPECT_TRUE(SocketAddress::V6(0).has_value());
    EXPECT_TRUE(SocketAddress::V6(65535).has_value());
}

TEST(SocketAddressTest, DefaultConstructedGettersAreEmpty)
{
    const SocketAddress a;
    EXPECT_FALSE(a.GetIp().has_value());
    EXPECT_FALSE(a.GetPort().has_value());
}

// ---- ResolveIp (blocking; init code only) ----

TEST(ResolveIpTest, NumericLiteralIsHermetic)
{
    auto r = ResolveIp("127.0.0.1", 80);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->GetIp().value_or("?"), "127.0.0.1");
    EXPECT_EQ(r->GetPort(), 80u);
}

TEST(ResolveIpTest, LocalhostMayResolveToV4OrV6)
{
    auto r = ResolveIp("localhost", 80);
    ASSERT_TRUE(r.has_value());
    const auto ip = r->GetIp().value_or("?");
    EXPECT_TRUE(ip == "127.0.0.1" || ip == "::1") << ip;
}

// getaddrinfo consumes a NUL-terminated C string: an embedded NUL used to
// silently resolve the TRUNCATED name. It must be rejected instead.
TEST(ResolveIpTest, EmbeddedNulIsRejected)
{
    const std::string sneaky{"127.0.0.1\0also-this", 19};
    auto r = ResolveIp(sneaky, 80);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().value(), EINVAL);
    EXPECT_EQ(r.error().context(), "ResolveIp");
}

// Network-dependent: only the failure is asserted, so a resolver that
// answers differently does not churn this test.
TEST(ResolveIpTest, InvalidHostFails)
{
    auto r = ResolveIp("x.invalid", 80);
    EXPECT_FALSE(r.has_value());
}

// ---- TcpListener ----

TEST(TcpListenerTest, BindLoopbackEphemeralPort)
{
    auto l = TcpListener::bind(0, "127.0.0.1");
    ASSERT_TRUE(l.has_value());
    ASSERT_TRUE(l->valid());

    EXPECT_GT(read_bound_port(l->get()), 0);
    EXPECT_EQ(read_ip(l->get()), "127.0.0.1");
}

TEST(TcpListenerTest, BindSetsTheDocumentedFlags)
{
    auto l = TcpListener::bind(0, "127.0.0.1");
    ASSERT_TRUE(l.has_value());

    const int flags = ::fcntl(l->get(), F_GETFL);
    ASSERT_NE(flags, -1);
    EXPECT_NE(flags & O_NONBLOCK, 0);

    const int fdflags = ::fcntl(l->get(), F_GETFD);
    ASSERT_NE(fdflags, -1);
    EXPECT_NE(fdflags & FD_CLOEXEC, 0);

    EXPECT_EQ(test::read_int_option(l->get(), SOL_SOCKET, SO_REUSEADDR), 1);
    EXPECT_EQ(test::read_int_option(l->get(), IPPROTO_TCP, TCP_NODELAY), 1);
}

// Bind() hardcodes ReusePort{true} (tcp_listener.cc), so a second listener
// on the same port succeeds: the review's "EADDRINUSE by default" scenario
// cannot happen through this API. Pinned as the actual contract -- if
// reuse_port ever becomes opt-out, this test must change.
TEST(TcpListenerTest, SecondBindToTheSamePortSucceedsViaReusePort)
{
    auto first = TcpListener::bind(0, "127.0.0.1");
    ASSERT_TRUE(first.has_value());
    const auto port = read_bound_port(first->get());

    auto second = TcpListener::bind(port, "127.0.0.1");
    ASSERT_TRUE(second.has_value()) << "Bind hardcodes SO_REUSEPORT; duplicate bind must succeed";
    EXPECT_EQ(read_bound_port(second->get()), port);
}

// 1000 failing binds must not leak the socket UniqueFd wrapped before the
// failure point.
TEST(TcpListenerTest, FailedBindsDoNotLeakFds)
{
    const auto before = test::OpenFdCount();
    for (int i = 0; i < 1000; ++i)
    {
        // TEST-NET-3: never assigned, never local -- bind fails EADDRNOTAVAIL.
        auto r = TcpListener::bind(0, "203.0.113.1");
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().value(), EADDRNOTAVAIL);
    }
    EXPECT_EQ(test::OpenFdCount(), before);
}

TEST(TcpListenerTest, InvalidIpStringIsRejected)
{
    auto r = TcpListener::bind(80, "not-an-ip");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().value(), EINVAL);
    EXPECT_EQ(r.error().context(), "inet_pton(AF_INET)");
}

// Docker grants CAP_NET_BIND_SERVICE by default, so privileged binds may
// succeed in the container; only assert the EACCES shape when they don't.
TEST(TcpListenerTest, BindPort80NeedsRoot)
{
    auto r = TcpListener::bind(80, "127.0.0.1");
    if (r.has_value())
    {
        GTEST_SKIP() << "CAP_NET_BIND_SERVICE present (docker default capability set)";
    }
    EXPECT_EQ(r.error().value(), EACCES);
}

TEST(TcpListenerTest, V6PathServesAnIpv6Socket)
{
    if (!ipv6_available())
    {
        GTEST_SKIP() << "no IPv6 support";
    }
    auto l = TcpListener::bind(0, "::1");
    ASSERT_TRUE(l.has_value());

    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    ASSERT_EQ(::getsockname(l->get(), reinterpret_cast<sockaddr*>(&ss), &len), 0);
    EXPECT_EQ(ss.ss_family, AF_INET6);
}
