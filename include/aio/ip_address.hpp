#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace aio::net
{

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

    /// @brief Returns the raw sockaddr pointer.
    [[nodiscard]] const sockaddr* Get() const { return reinterpret_cast<const sockaddr*>(&addr); }

    /// @brief Returns the raw sockaddr pointer (mutable).
    [[nodiscard]] sockaddr* GetMutable() { return reinterpret_cast<sockaddr*>(&addr); }

    /// @brief Retrieves the IP address as a string.
    [[nodiscard]] std::optional<std::string> GetIp() const
    {
        char buffer[INET6_ADDRSTRLEN];
        if (addr.ss_family == AF_INET)
        {
            const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
            if (inet_ntop(AF_INET, &in->sin_addr, buffer, sizeof(buffer)))
            {
                return std::string(buffer);
            }
        }
        else if (addr.ss_family == AF_INET6)
        {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
            if (inet_ntop(AF_INET6, &in6->sin6_addr, buffer, sizeof(buffer)))
            {
                return std::string(buffer);
            }
        }
        return std::nullopt;
    }

    /// @brief Retrieves the port number (host byte order).
    [[nodiscard]] std::optional<uint16_t> GetPort() const
    {
        if (addr.ss_family == AF_INET)
        {
            const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
            return ntohs(in->sin_port);
        }
        else if (addr.ss_family == AF_INET6)
        {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
            return ntohs(in6->sin6_port);
        }
        return std::nullopt;
    }
};

}  // namespace aio::net