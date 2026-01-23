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

/// @brief RAII wrapper for socket file descriptors.
///
/// Automatically closes the socket on destruction. Move-only to prevent
/// double-close bugs. Use with async I/O operations via the Get() method.
///
/// @code
///   auto result = TcpListener::Bind(8080);
///   if (!result) return;
///   Socket server = std::move(*result);
///
///   while (running) {
///       auto client_fd = co_await AsyncAccept(ctx, server.Get());
///       // ...
///   }
/// @endcode
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

    /// @brief Returns the raw file descriptor.
    /// @note The Socket retains ownership. Do not close the returned fd directly.
    [[nodiscard]] int Get() const { return fd_; }

    /// @brief Checks if the socket holds a valid file descriptor.
    [[nodiscard]] bool IsValid() const { return fd_ >= 0; }

    /// @brief Boolean conversion for validity checks.
    explicit operator bool() const { return IsValid(); }

    /// @brief Releases ownership of the file descriptor.
    /// @return The raw file descriptor. Caller is responsible for closing it.
    /// @warning After calling Release(), the Socket is empty and must not be used.
    int Release() { return std::exchange(fd_, -1); }

    /// @brief Closes the socket explicitly (before destruction).
    /// @note Safe to call multiple times; subsequent calls are no-ops.
    void Close();

    // --- Socket Options ---

    /// @brief Sets the socket to non-blocking mode.
    /// @return Result<void> - success or error code
    /// @note Required for use with io_uring async operations.
    Result<void> SetNonBlocking() const;

    /// @brief Enables/disables SO_REUSEADDR.
    /// @param enable True to enable (default), false to disable
    /// @return Result<void> - success or error code
    /// @note Allows binding to a port in TIME_WAIT state.
    Result<> SetReuseAddr(bool enable = true) const;

    /// @brief Enables/disables SO_REUSEPORT.
    /// @param enable True to enable (default), false to disable
    /// @return Result<void> - success or error code
    /// @note Allows multiple sockets to bind to the same port for load balancing.
    Result<> SetReusePort(bool enable = true) const;

    /// @brief Enables/disables TCP_NODELAY (Nagle's algorithm).
    /// @param enable True to disable Nagle (low latency), false to enable Nagle
    /// @return Result<void> - success or error code
    /// @note Enable for latency-sensitive protocols; disable for bulk transfers.
    Result<> SetNodelay(bool enable = true) const;

    /// @brief Sets the socket send buffer size (SO_SNDBUF).
    /// @param size Buffer size in bytes
    /// @return Result<void> - success or error code
    Result<> SetSendBuffer(int size) const;

    /// @brief Sets the socket receive buffer size (SO_RCVBUF).
    /// @param size Buffer size in bytes
    /// @return Result<void> - success or error code
    Result<> SetRecvBuffer(int size) const;
};

////////////////////////////////////////////////////////////////////////////////
// SocketAddress - IPv4/IPv6 wrapper
////////////////////////////////////////////////////////////////////////////////

/// @brief Wrapper for sockaddr_storage supporting both IPv4 and IPv6.
///
/// Provides convenient factory methods for creating addresses and async DNS
/// resolution that doesn't block the event loop.
///
/// @code
///   // Direct IPv4 address
///   auto addr = SocketAddress::V4(8080, "0.0.0.0");
///
///   // Async DNS resolution (non-blocking)
///   auto addr = co_await SocketAddress::ResolveAsync(ctx, pool, "example.com", 443);
/// @endcode
struct SocketAddress
{
    sockaddr_storage addr{};
    socklen_t addrlen = sizeof(sockaddr_storage);

    SocketAddress() = default;

    /// @brief Creates an IPv4 address.
    /// @param port Port number in host byte order (automatically converted to network order)
    /// @param ip IPv4 address string (e.g., "127.0.0.1"). Pass nullptr for INADDR_ANY (0.0.0.0).
    /// @return SocketAddress configured for IPv4
    ///
    /// @code
    ///   auto any = SocketAddress::V4(8080);              // Bind to all interfaces
    ///   auto local = SocketAddress::V4(8080, "127.0.0.1"); // Localhost only
    /// @endcode
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

    /// @brief Creates an IPv6 address.
    /// @param port Port number in host byte order (automatically converted to network order)
    /// @param ip IPv6 address string (e.g., "::1"). Pass nullptr for in6addr_any (::).
    /// @return SocketAddress configured for IPv6
    ///
    /// @code
    ///   auto any = SocketAddress::V6(8080);         // Bind to all IPv6 interfaces
    ///   auto local = SocketAddress::V6(8080, "::1"); // IPv6 localhost only
    /// @endcode
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

    /// @brief Synchronously resolves a hostname to an address (BLOCKING).
    /// @param host Hostname to resolve (e.g., "example.com")
    /// @param port Port number in host byte order
    /// @return Result<SocketAddress> with resolved address or error
    ///
    /// @warning This is a BLOCKING call that may take seconds for DNS resolution.
    ///          Use ResolveAsync() in async code paths to avoid blocking the event loop.
    ///
    /// @code
    ///   // OK in initialization code
    ///   auto addr = SocketAddress::Resolve("database.local", 5432);
    /// @endcode
    static Result<SocketAddress> Resolve(std::string_view host, uint16_t port);

    /// @brief Asynchronously resolves a hostname without blocking the event loop.
    /// @param ctx The IoContext to run on
    /// @param pool BlockingPool to offload the DNS resolution
    /// @param host Hostname to resolve (copied internally)
    /// @param port Port number in host byte order
    /// @return Task<Result<SocketAddress>> with resolved address or error
    ///
    /// @note Uses the blocking pool to run getaddrinfo() off the event loop thread.
    ///       The hostname is captured by value to ensure it survives the thread switch.
    ///
    /// @code
    ///   auto addr = co_await SocketAddress::ResolveAsync(ctx, pool, "api.example.com", 443);
    ///   if (!addr) {
    ///       // DNS resolution failed
    ///   }
    ///   auto result = co_await AsyncConnect(ctx, socket, addr->Get(), addr->addrlen);
    /// @endcode
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

/// @brief Factory for creating TCP listening sockets with high-performance defaults.
///
/// Automatically configures SO_REUSEADDR, SO_REUSEPORT, TCP_NODELAY, and non-blocking mode.
/// The socket is ready for use with AsyncAccept() after creation.
///
/// @code
///   auto listener = TcpListener::Bind(8080);
///   if (!listener) {
///       // Handle error
///   }
///   Socket server = std::move(*listener);
///
///   while (running) {
///       auto client = co_await AsyncAccept(ctx, server.Get());
///       // Handle connection...
///   }
/// @endcode
struct TcpListener
{
    /// @brief Creates, binds, and listens on a socket with high-performance defaults.
    /// @param addr The SocketAddress to bind to (IPv4 or IPv6)
    /// @param backlog Pending connection queue size (default: 4096)
    /// @return Result<Socket> with the listening socket or error
    ///
    /// @note Automatically sets: SO_REUSEADDR, SO_REUSEPORT, TCP_NODELAY, non-blocking.
    ///       The returned socket is immediately ready for AsyncAccept().
    static Result<Socket> Bind(const SocketAddress& addr, int backlog = 4096);

    /// @brief Convenience overload that binds to all interfaces on the given port.
    /// @param port Port number in host byte order
    /// @return Result<Socket> with the listening socket or error
    ///
    /// @note Equivalent to Bind(SocketAddress::V4(port), 4096).
    ///
    /// @code
    ///   auto server = TcpListener::Bind(8080);  // Bind to 0.0.0.0:8080
    /// @endcode
    static Result<Socket> Bind(uint16_t port);
};

}  // namespace aio::net
