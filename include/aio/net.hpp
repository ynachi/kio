#pragma once
////////////////////////////////////////////////////////////////////////////////
// Network utilities for kio
//
// Consolidates Socket RAII, Address resolution, and TCP setup helpers.
// Optimized for low-overhead server applications.
////////////////////////////////////////////////////////////////////////////////

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

#include <sys/socket.h>

#include "IoContext.hpp"
#include "aio/BlockingPool.hpp"
#include "aio/result.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>

namespace aio::net
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

    ~Socket() noexcept
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

    [[nodiscard]] int Get() const { return fd_; }
    [[nodiscard]] bool IsValid() const { return fd_ >= 0; }
    explicit operator bool() const { return IsValid(); }

    /// @brief Releases ownership of the file descriptor.
    /// @return The raw file descriptor. Caller must close it.
    int Release() { return std::exchange(fd_, -1); }

    /// @brief Close the socket explicitly.
    void Close();

    // --- Socket Options ---

    Result<void> SetNonBlocking() const;
    Result<> SetReuseAddr(bool enable = true) const;
    Result<> SetReusePort(bool enable = true) const;
    Result<> SetNodelay(bool enable = true) const;
    Result<> SetSendBuffer(int size) const;
    Result<> SetRecvBuffer(int size) const;
};

////////////////////////////////////////////////////////////////////////////////
// SocketAddress - IPv4/IPv6 wrapper
////////////////////////////////////////////////////////////////////////////////
struct SocketAddress
{
    sockaddr_storage addr{};
    socklen_t addrlen = sizeof(sockaddr_storage);

    SocketAddress() = default;

    /// @brief Creates an IPv4 address helper.
    /// @param port The port number (host byte order).
    /// @param ip The IP string (e.g., "127.0.0.1"). If null, uses INADDR_ANY.
    static SocketAddress V4(uint16_t port, const char* ip = nullptr)
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

    /// @brief Creates an IPv6 address helper.
    /// @param port The port number (host byte order).
    /// @param ip The IP string (e.g., "::1"). If null, uses in6addr_any.
    static SocketAddress V6(uint16_t port, const char* ip = nullptr)
    {
        SocketAddress sa;
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&sa.addr);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        if (ip && *ip)
        {
            inet_pton(AF_INET6, ip, &in6->sin6_addr);
        }
        else
        {
            in6->sin6_addr = in6addr_any;
        }
        sa.addrlen = sizeof(sockaddr_in6);
        return sa;
    }

    /// @brief Synchronously resolves a hostname to an address.
    /// @param host The hostname (e.g., "google.com").
    /// @param port The port number.
    /// @return A Result containing the SocketAddress.
    /// @note This is a BLOCKING call. Use resolve_async in hot paths.
    static Result<SocketAddress> Resolve(std::string_view host, uint16_t port);

    /// @brief Asynchronously resolves a hostname without blocking the reactor.
    /// @param ctx The io context to use.
    /// @param pool The blocking pool used to offload the resolution.
    /// @param host The hostname.
    /// @param port The port number.
    /// @return A Task containing the resolved SocketAddress.
    static Task<Result<SocketAddress>> ResolveAsync(IoContext& ctx, BlockingPool& pool, std::string host, uint16_t port)
    {
        // We capture 'host' by value (std::string) to ensure it survives the thread switch
        auto result = co_await Offload(ctx, pool, [h = std::move(host), port]() -> Result<SocketAddress> {
            // This runs on a background thread, so blocking is fine
            return Resolve(h, port);
        });

        co_return result;
    }

    const sockaddr* Get() const { return reinterpret_cast<const sockaddr*>(&addr); }
};

////////////////////////////////////////////////////////////////////////////////
// TcpListener - Factory for server sockets
////////////////////////////////////////////////////////////////////////////////
struct TcpListener
{
    /// @brief Creates, binds, and listens on a socket with high-performance defaults.
    /// @param addr The address to bind to.
    /// @param backlog Pending connection queue size (default 4096).
    /// @return A Result containing the bound listening Socket.
    static Result<Socket> Bind(const SocketAddress& addr, int backlog = 4096);

    static Result<Socket> Bind(uint16_t port);
};

}  // namespace aio::net
