#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>

#include "../../include/aio/aio.hpp"
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>

using namespace std::chrono_literals;

namespace {

int make_temp_fd() {
    char path[] = "/tmp/aio_testXXXXXX";
    int fd = ::mkstemp(path);
    if (fd >= 0) {
        ::unlink(path);
    }
    return fd;
}

template <typename T>
T run_with_timeout(aio::io_context& ctx, aio::task<T>& t, std::chrono::milliseconds timeout) {
    t.start();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    ctx.run([&] {
        if (t.done() || std::chrono::steady_clock::now() >= deadline) {
            ctx.stop();
        }
    });
    if (!t.done()) {
        throw std::runtime_error("aio task timed out");
    }
    return t.result();
}

struct FileRwResult {
    aio::Result<size_t> written;
    aio::Result<size_t> read;
    std::array<std::byte, 5> out;
};

aio::task<FileRwResult> file_rw_task(aio::io_context& ctx, int fd) {
    const std::array<std::byte, 5> data{
        std::byte{static_cast<unsigned char>('h')},
        std::byte{static_cast<unsigned char>('e')},
        std::byte{static_cast<unsigned char>('l')},
        std::byte{static_cast<unsigned char>('l')},
        std::byte{static_cast<unsigned char>('o')},
    };
    auto written = co_await aio::async_write(ctx, fd, std::span<const std::byte>(data), 0);

    std::array<std::byte, 5> out{};
    auto read = co_await aio::async_read(ctx, fd, std::span<std::byte>(out), 0);

    co_return FileRwResult{written, read, out};
}

struct SendRecvResult {
    aio::Result<size_t> sent;
    aio::Result<size_t> received;
    std::string payload;
};

aio::task<SendRecvResult> send_recv_task(aio::io_context& ctx, int fd_send, int fd_recv) {
    const std::string msg = "ping";
    auto sent = co_await aio::async_send(ctx, fd_send, std::string_view{msg});

    char buf[16] = {};
    auto received = co_await aio::async_recv(ctx, fd_recv, buf).with_timeout(100ms);

    std::string payload;
    if (received) {
        payload.assign(buf, buf + *received);
    }

    co_return SendRecvResult{sent, received, payload};
}

aio::task<aio::Result<uint64_t>> event_timeout_task(aio::io_context& ctx, aio::event& evt) {
    co_return co_await evt.wait().with_timeout(10ms);
}

aio::task<int> offload_task(aio::io_context& ctx, aio::blocking_pool& pool) {
    auto value = co_await aio::offload(ctx, pool, [] { return 42; });
    co_return value;
}

}  // namespace

TEST(SocketAddressTest, V6Any) {
    const auto sa = aio::net::SocketAddress::v6(8080);
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&sa.addr);
    EXPECT_EQ(in6->sin6_family, AF_INET6);
    EXPECT_EQ(ntohs(in6->sin6_port), 8080);
    EXPECT_EQ(sa.addrlen, sizeof(sockaddr_in6));
}

TEST(SocketAddressTest, V6Loopback) {
    const auto sa = aio::net::SocketAddress::v6(8080, "::1");
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&sa.addr);
    EXPECT_TRUE(IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr));
}

TEST(AioEventTest, WaitTimeout) {
    aio::io_context ctx;
    aio::event evt{&ctx};

    auto t = event_timeout_task(ctx, evt);
    auto result = run_with_timeout(ctx, t, 200ms);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::make_error_code(std::errc::timed_out));
}

TEST(AioBlockingPoolTest, Offload) {
    aio::io_context ctx;
    aio::blocking_pool pool(1);

    auto t = offload_task(ctx, pool);
    const int result = run_with_timeout(ctx, t, 500ms);

    EXPECT_EQ(result, 42);
}

TEST(AioIoTest, SendRecvSocketpair) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    aio::io_context ctx;
    auto t = send_recv_task(ctx, fds[0], fds[1]);
    auto result = run_with_timeout(ctx, t, 500ms);

    ASSERT_TRUE(result.sent);
    ASSERT_TRUE(result.received);
    EXPECT_EQ(result.payload, "ping");

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(AioIoTest, ReadWriteFile) {
    const int fd = make_temp_fd();
    ASSERT_GE(fd, 0);

    aio::io_context ctx;
    auto t = file_rw_task(ctx, fd);
    auto result = run_with_timeout(ctx, t, 500ms);

    ASSERT_TRUE(result.written);
    ASSERT_TRUE(result.read);
    EXPECT_EQ(*result.written, result.out.size());
    EXPECT_EQ(*result.read, result.out.size());

    std::string out(reinterpret_cast<const char*>(result.out.data()), result.out.size());
    EXPECT_EQ(out, "hello");

    ::close(fd);
}
