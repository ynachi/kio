#pragma once

#include "fd.hpp"
#include "net.hpp"

namespace URing
{
struct TcpListener
{
    /// @brief Creates, binds, and listens on a socket with high-performance defaults.
    /// @param addr The SocketAddress to bind to (IPv4 or IPv6)
    /// @param backlog Pending connection queue size (default: 4096)
    /// @return Result<Fd> with the listening socket or error
    static Result<Fd> Bind(const SocketAddress& addr, int backlog = 4096);

    /// @brief Convenience overload that automatically detects IPv4 vs IPv6.
    /// @param port Port number in host byte order
    /// @param ip IP string (e.g., "0.0.0.0", "127.0.0.1", or "::1").
    ///           Defaults to nullptr (INADDR_ANY / 0.0.0.0).
    /// @param backlog Pending connection queue size
    /// @return Result<Fd> with the listening socket or error
    static Result<Fd> Bind(uint16_t port, const char* ip = nullptr, int backlog = 4096);
};
}  // namespace URing