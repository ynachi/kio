#pragma once
#include <optional>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "error.hpp"
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace URing
{
template <typename Opt>
concept SocketOption = requires(const Opt& o, int fd) {
    { o.Apply(fd) } noexcept -> std::same_as<Result<void>>;
};

// ----------------------------------------------------------
// Options
// ----------------------------------------------------------
namespace SockOpt
{
template <int Level, int Name, typename T = int>
struct BasicOption
{
    T value;
    explicit BasicOption(T v) noexcept : value(v) {}

    [[nodiscard]] Result<void> Apply(const int fd) const noexcept
    {
        if (::setsockopt(fd, Level, Name, &value, sizeof(value)) < 0)
            return ErrorFromErrno(errno);
        return {};
    }
};

struct ReuseAddr : BasicOption<SOL_SOCKET, SO_REUSEADDR>
{
    explicit ReuseAddr(const bool e = true) noexcept : BasicOption(e ? 1 : 0) {}
};

struct ReusePort : BasicOption<SOL_SOCKET, SO_REUSEPORT>
{
    explicit ReusePort(const bool e = true) noexcept : BasicOption(e ? 1 : 0) {}
};

struct NoDelay : BasicOption<IPPROTO_TCP, TCP_NODELAY>
{
    explicit NoDelay(const bool e = true) noexcept : BasicOption(e ? 1 : 0) {}
};

struct SendBuffer : BasicOption<SOL_SOCKET, SO_SNDBUF>
{
    explicit SendBuffer(const int n) noexcept : BasicOption(n) {}
};

struct RecvBuffer : BasicOption<SOL_SOCKET, SO_RCVBUF>
{
    explicit RecvBuffer(const int n) noexcept : BasicOption(n) {}
};

struct KeepAlive : BasicOption<SOL_SOCKET, SO_KEEPALIVE>
{
    explicit KeepAlive(const bool e = true) noexcept : BasicOption(e ? 1 : 0) {}
};

struct NonBlocking
{
    bool enable;
    explicit NonBlocking(const bool e = true) noexcept : enable(e) {}

    [[nodiscard]] Result<void> Apply(const int fd) const noexcept
    {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1)
            return ErrorFromErrno(errno);
        flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        if (::fcntl(fd, F_SETFL, flags) == -1)
            return ErrorFromErrno(errno);
        return {};
    }
};
}  // namespace SockOpt
////////////////////////////////////////////////////////////////////////////////
// SocketAddress - IPv4/IPv6 wrapper, data only
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
    static SocketAddress V4(std::uint16_t port, const char* ip = nullptr);

    /// @brief Creates an IPv6 address.
    /// @param port Port number in host byte order (automatically converted to network order)
    /// @param ip IPv6 address string (e.g., "::1"). Pass nullptr for in6addr_any (::).
    /// @return SocketAddress configured for IPv6
    ///
    /// @code
    ///   auto any = SocketAddress::V6(8080);         // Bind to all IPv6 interfaces
    ///   auto local = SocketAddress::V6(8080, "::1"); // IPv6 localhost only
    /// @endcode
    static SocketAddress V6(uint16_t port, const char* ip = nullptr);

    /// @brief Returns the raw sockaddr pointer.
    [[nodiscard]] const sockaddr* Get() const { return reinterpret_cast<const sockaddr*>(&addr); }

    /// @brief Returns the raw sockaddr pointer (mutable).
    [[nodiscard]] sockaddr* GetMutable() { return reinterpret_cast<sockaddr*>(&addr); }

    /// @brief Retrieves the IP address as a string.
    [[nodiscard]] std::optional<std::string> GetIp() const;

    /// @brief Retrieves the port number (host byte order).
    [[nodiscard]] std::optional<uint16_t> GetPort() const;
};

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
static Result<SocketAddress> ResolveIp(std::string_view host, uint16_t port);

}  // namespace URing