// tests/aio/net_tests.cpp
// Tests for networking utilities (Socket, SocketAddress, TcpListener)

#include <chrono>
#include <thread>

#include <unistd.h>

#include <sys/socket.h>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/net.hpp"
#include "test_helpers.hpp"
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>

using namespace aio;
using namespace aio::net;
using namespace aio::test;
using namespace std::chrono_literals;

// -----------------------------------------------------------------------------
// SocketAddress Tests
// -----------------------------------------------------------------------------

TEST(SocketAddressTest, V4Any) {
    const auto sa = SocketAddress::V4(8080);
    const auto* in = reinterpret_cast<const sockaddr_in*>(&sa.addr);

    EXPECT_EQ(in->sin_family, AF_INET);
    EXPECT_EQ(ntohs(in->sin_port), 8080);
    EXPECT_EQ(in->sin_addr.s_addr, INADDR_ANY);
    EXPECT_EQ(sa.addrlen, sizeof(sockaddr_in));
}

TEST(SocketAddressTest, V4Localhost) {
    const auto sa = SocketAddress::V4(9000, "127.0.0.1");
    const auto* in = reinterpret_cast<const sockaddr_in*>(&sa.addr);

    EXPECT_EQ(in->sin_family, AF_INET);
    EXPECT_EQ(ntohs(in->sin_port), 9000);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in->sin_addr, ip_str, sizeof(ip_str));
    EXPECT_STREQ(ip_str, "127.0.0.1");
}

TEST(SocketAddressTest, V4SpecificAddress) {
    const auto sa = SocketAddress::V4(80, "192.168.1.100");
    const auto* in = reinterpret_cast<const sockaddr_in*>(&sa.addr);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in->sin_addr, ip_str, sizeof(ip_str));
    EXPECT_STREQ(ip_str, "192.168.1.100");
}

TEST(SocketAddressTest, V6Any) {
    const auto sa = SocketAddress::V6(8080);
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&sa.addr);

    EXPECT_EQ(in6->sin6_family, AF_INET6);
    EXPECT_EQ(ntohs(in6->sin6_port), 8080);
    EXPECT_EQ(sa.addrlen, sizeof(sockaddr_in6));
}

TEST(SocketAddressTest, V6Loopback) {
    const auto sa = SocketAddress::V6(8080, "::1");
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&sa.addr);

    EXPECT_TRUE(IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr));
}

TEST(SocketAddressTest, V6SpecificAddress) {
    const auto sa = SocketAddress::V6(443, "2001:db8::1");
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&sa.addr);

    char ip_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &in6->sin6_addr, ip_str, sizeof(ip_str));
    EXPECT_STREQ(ip_str, "2001:db8::1");
}

TEST(SocketAddressTest, GetPointer) {
    const auto sa = SocketAddress::V4(8080);
    const sockaddr* ptr = sa.Get();

    EXPECT_EQ(ptr->sa_family, AF_INET);
}

// -----------------------------------------------------------------------------
// Socket RAII Tests
// -----------------------------------------------------------------------------

TEST(SocketTest, DefaultConstruction) {
    Socket s;
    EXPECT_FALSE(s.IsValid());
    EXPECT_EQ(s.Get(), -1);
}

TEST(SocketTest, ConstructFromFd) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket s1(fds[0]);
    Socket s2(fds[1]);

    EXPECT_TRUE(s1.IsValid());
    EXPECT_TRUE(s2.IsValid());
    EXPECT_EQ(s1.Get(), fds[0]);
    EXPECT_EQ(s2.Get(), fds[1]);
}

TEST(SocketTest, MoveConstruction) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket s1(fds[0]);
    Socket s2(std::move(s1));

    EXPECT_FALSE(s1.IsValid());  // s1 is moved-from
    EXPECT_TRUE(s2.IsValid());
    EXPECT_EQ(s2.Get(), fds[0]);

    ::close(fds[1]);
}

TEST(SocketTest, MoveAssignment) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket s1(fds[0]);
    Socket s2;

    s2 = std::move(s1);

    EXPECT_FALSE(s1.IsValid());
    EXPECT_TRUE(s2.IsValid());
    EXPECT_EQ(s2.Get(), fds[0]);

    ::close(fds[1]);
}

TEST(SocketTest, Release) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket s(fds[0]);
    int released = s.Release();

    EXPECT_EQ(released, fds[0]);
    EXPECT_FALSE(s.IsValid());

    // We must close manually now
    ::close(released);
    ::close(fds[1]);
}

TEST(SocketTest, ExplicitClose) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket s(fds[0]);
    EXPECT_TRUE(s.IsValid());

    s.Close();

    EXPECT_FALSE(s.IsValid());
    EXPECT_EQ(s.Get(), -1);

    // Close again should be safe
    s.Close();

    ::close(fds[1]);
}

TEST(SocketTest, BoolConversion) {
    Socket empty;
    EXPECT_FALSE(static_cast<bool>(empty));

    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket valid(fds[0]);
    EXPECT_TRUE(static_cast<bool>(valid));

    ::close(fds[1]);
}

// -----------------------------------------------------------------------------
// Socket Options Tests
// -----------------------------------------------------------------------------

TEST(SocketOptionsTest, SetNonBlocking) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket s(fds[0]);
    auto result = s.SetNonBlocking();
    EXPECT_TRUE(result.has_value());

    ::close(fds[1]);
}

TEST(SocketOptionsTest, SetReuseAddr) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    Socket s(fd);
    auto result = s.SetReuseAddr(true);
    EXPECT_TRUE(result.has_value());
}

TEST(SocketOptionsTest, SetReusePort) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    Socket s(fd);
    auto result = s.SetReusePort(true);
    EXPECT_TRUE(result.has_value());
}

TEST(SocketOptionsTest, SetNodelay) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    Socket s(fd);
    auto result = s.SetNodelay(true);
    EXPECT_TRUE(result.has_value());
}

// -----------------------------------------------------------------------------
// TcpListener Tests
// -----------------------------------------------------------------------------

TEST(TcpListenerTest, BindToPort) {
    // Use port 0 to let the OS assign a free port
    auto addr = SocketAddress::V4(0, "127.0.0.1");
    auto result = TcpListener::Bind(addr);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());
}

TEST(TcpListenerTest, BindPortOnly) {
    // Convenience overload - also use port 0
    // Note: This test might fail if port is already in use
    auto result = TcpListener::Bind(0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());
}

TEST(TcpListenerTest, AcceptConnection) {
    IoContext ctx;

    auto listener_result = TcpListener::Bind(SocketAddress::V4(0, "127.0.0.1"));
    ASSERT_TRUE(listener_result.has_value());
    Socket listener = std::move(*listener_result);

    // Get the actual port
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&addr), &len), 0);
    uint16_t port = ntohs(addr.sin_port);

    // Start a thread to connect
    std::thread client([port]() {
        std::this_thread::sleep_for(10ms);

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

        ::connect(fd, reinterpret_cast<sockaddr*>(&server), sizeof(server));
        ::close(fd);
    });

    auto test = [&]() -> Task<> {
        auto accept_result = co_await AsyncAccept(ctx, listener.Get())
            .WithTimeout(1s);

        EXPECT_TRUE(accept_result.has_value());
        if (accept_result.has_value()) {
            ::close(*accept_result);
        }
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
    client.join();
}

// -----------------------------------------------------------------------------
// AsyncConnect Tests
// -----------------------------------------------------------------------------

TEST(AsyncConnectTest, ConnectToListener) {
    IoContext ctx;

    // Create listener
    auto listener_result = TcpListener::Bind(SocketAddress::V4(0, "127.0.0.1"));
    ASSERT_TRUE(listener_result.has_value());
    Socket listener = std::move(*listener_result);

    // Get port
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ::getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);

    // Create client socket
    int client_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_GE(client_fd, 0);
    FdGuard client(client_fd);

    // Accept in background
    std::thread acceptor([&]() {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int accepted = ::accept(listener.Get(), reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (accepted >= 0) ::close(accepted);
    });

    auto server_addr = SocketAddress::V4(port, "127.0.0.1");

    auto test = [&]() -> Task<void> {
        auto result = co_await AsyncConnect(ctx, client_fd, server_addr.Get(), server_addr.addrlen)
            .WithTimeout(1s);

        EXPECT_TRUE(result.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
    acceptor.join();
}

TEST(AsyncConnectTest, ConnectRefused) {
    IoContext ctx;

    // Create socket
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_GE(fd, 0);
    FdGuard client(fd);

    // Try to connect to a port that's not listening
    // Port 1 is typically not available to regular users
    auto addr = SocketAddress::V4(1, "127.0.0.1");

    auto test = [&]() -> Task<void> {
        auto result = co_await AsyncConnect(ctx, fd, addr.Get(), addr.addrlen)
            .WithTimeout(1s);

        // Should fail with connection refused or permission denied
        EXPECT_FALSE(result.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

