#pragma once
////////////////////////////////////////////////////////////////////////////////
// Network utilities for uring executor
//
// Consolidates Socket RAII, Address resolution, and TCP setup helpers.
// Optimized for low-overhead server applications.
////////////////////////////////////////////////////////////////////////////////

#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <sys/socket.h>

#include "result.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// forward declaration for ThreadContext
namespace uring { class ThreadContext; }

namespace uring::net
{

////////////////////////////////////////////////////////////////////////////////
// Socket - RAII wrapper for file descriptors
////////////////////////////////////////////////////////////////////////////////
class Socket
{
    int fd_ = -1;

public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}

    ~Socket()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    // Move-only
    Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Socket& operator=(Socket&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool is_valid() const { return fd_ >= 0; }
    explicit operator bool() const { return is_valid(); }

    // Release ownership (caller must close)
    int release() { return std::exchange(fd_, -1); }

    // Close explicitly
    void close()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // --- Socket Options ---

    Result<void> set_non_blocking() const
    {
        int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags == -1)
            return error_from_errno(errno);
        if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1)
            return error_from_errno(errno);
        return {};
    }

    Result<> set_reuse_addr(bool enable = true) const
    {
        int opt = enable ? 1 : 0;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
            return error_from_errno(errno);
        return {};
    }

    Result<> set_reuse_port(bool enable = true) const
    {
        int opt = enable ? 1 : 0;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
            return error_from_errno(errno);
        return {};
    }

    Result<> set_nodelay(bool enable = true) const
    {
        int opt = enable ? 1 : 0;
        if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0)
            return error_from_errno(errno);
        return {};
    }

    Result<> set_send_buffer(int size) const
    {
        if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
            return error_from_errno(errno);
        return {};
    }

    Result<> set_recv_buffer(int size) const
    {
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)
            return error_from_errno(errno);
        return {};
    }
};

////////////////////////////////////////////////////////////////////////////////
// SocketAddress - IPv4/IPv6 wrapper
////////////////////////////////////////////////////////////////////////////////
struct SocketAddress
{
    sockaddr_storage addr{};
    socklen_t addrlen = sizeof(sockaddr_storage);

    SocketAddress() = default;

    // Helper for IPv4 loopback/any
    static SocketAddress v4(uint16_t port, const char* ip = nullptr)
    {
        SocketAddress sa;
        auto* in = reinterpret_cast<sockaddr_in*>(&sa.addr);
        in->sin_family = AF_INET;
        in->sin_port = htons(port);
        if (ip && *ip)
        {
            inet_pton(AF_INET, ip, &in->sin_addr);
        }
        else
        {
            in->sin_addr.s_addr = INADDR_ANY;
        }
        sa.addrlen = sizeof(sockaddr_in);
        return sa;
    }

    // Resolves hostname (Synchronous - OK for now)
    static Result<SocketAddress> resolve(std::string_view host, uint16_t port)
    {
        addrinfo hints{}, *res;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        std::string service = std::to_string(port);
        std::string hostname(host);  // getaddrinfo needs null-terminated

        if (int rc = getaddrinfo(hostname.c_str(), service.c_str(), &hints, &res); rc != 0)
        {
            // getaddrinfo returns EAI_* errors, not errno, but we map to std::error_code generically
            return std::unexpected(std::make_error_code(std::errc::address_not_available));
        }

        SocketAddress out;
        if (res)
        {
            std::memcpy(&out.addr, res->ai_addr, res->ai_addrlen);
            out.addrlen = res->ai_addrlen;
            freeaddrinfo(res);
        }
        return out;
    }

    // Async Resolution (Non-blocking)
    // Requires the ThreadContext to offload the work
    static Task<Result<SocketAddress>> resolve_async(ThreadContext& ctx, std::string host, uint16_t port)
    {
        // We capture 'host' by value (std::string) to ensure it survives the thread switch
        auto result = co_await ctx.spawn_blocking([h = std::move(host), port]() -> SocketAddress {
            // This runs on a background thread, so blocking is fine
            auto res = resolve(h, port);
            if (!res) throw std::runtime_error("resolution failed"); // Caught by spawn_blocking
            return *res;
        });

        co_return result;
    }

    const sockaddr* get() const { return reinterpret_cast<const sockaddr*>(&addr); }
};

////////////////////////////////////////////////////////////////////////////////
// TcpListener - Factory for server sockets
////////////////////////////////////////////////////////////////////////////////
struct TcpListener
{
    // Creates a bound, non-blocking, listening socket
    static Result<Socket> bind(const SocketAddress& addr, int backlog = 4096)
    {
        int fd = ::socket(addr.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return error_from_errno(errno);

        Socket sock(fd);

        // Standard high-perf defaults
        if (auto r = sock.set_reuse_addr(); !r)
            return std::unexpected(r.error());
        if (auto r = sock.set_reuse_port(); !r)
            return std::unexpected(r.error());

        // Explicit bind
        if (::bind(fd, addr.get(), addr.addrlen) < 0)
        {
            return error_from_errno(errno);
        }

        if (::listen(fd, backlog) < 0)
        {
            return error_from_errno(errno);
        }

        return sock;
    }

    static Result<Socket> bind(uint16_t port) { return bind(SocketAddress::v4(port)); }
};

}  // namespace uring::net