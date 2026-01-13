//
// Created by Yao ACHI on 06/10/2025.
//

#ifndef KIO_NET_H
#define KIO_NET_H
#include <arpa/inet.h>
#include <expected>
#include <string_view>
#include <unistd.h>

#include "../core/errors.h"

namespace kio::net
{
struct SocketAddress
{
    std::string ip;
    uint16_t port{};
    int family{};

    sockaddr_storage addr{};
    socklen_t addrlen{};

    // Extract ip / port / family from addr + addrlen
    bool populate_from_storage() noexcept
    {
        if (addrlen == 0) return false;

        family = addr.ss_family;

        char ipbuf[INET6_ADDRSTRLEN];

        switch (family)
        {
            case AF_INET:
            {
                const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);

                if (!inet_ntop(AF_INET, &in->sin_addr, ipbuf, sizeof(ipbuf))) return false;

                ip = ipbuf;
                port = ntohs(in->sin_port);
                return true;
            }

            case AF_INET6:
            {
                const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);

                if (!inet_ntop(AF_INET6, &in6->sin6_addr, ipbuf, sizeof(ipbuf))) return false;

                ip = ipbuf;
                port = ntohs(in6->sin6_port);
                return true;
            }

            default:
                return false;
        }
    }

    [[nodiscard]] const sockaddr* as_sockaddr() const noexcept { return reinterpret_cast<const sockaddr*>(&addr); }
    [[nodiscard]] sockaddr* as_sockaddr_mut() noexcept { return reinterpret_cast<sockaddr*>(&addr); }
};

struct FDGuard
{
    int fd = -1;
    explicit FDGuard(const int f) : fd(f) {}
    ~FDGuard()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        };
    }
    FDGuard(FDGuard&& other) noexcept : fd(other.fd) { other.fd = -1; }
    FDGuard(const FDGuard&) = delete;
    [[nodiscard]] int get() const { return fd; }
};

Result<int> create_tcp_fd(int family);
Result<void> resolve_address(std::string_view hostname, uint16_t port, sockaddr_storage& out);
Result<void> set_tcp_fd_options(int fd);

//===========
// TODO: Review the methods below, we might have to remove them
// =============
/**
 * Creates a TCP socket and start listening on it. This socket is set as reuse and non-blocking
 * @param ip_address
 * @param port
 * @param backlog
 * @return
 */
Result<int> create_tcp_server_socket(std::string_view ip_address, uint16_t port, int backlog);
Result<void> listen_on_sock(int fd, const SocketAddress& addr, int backlog);
// set so_reuse on sockets
Result<void> set_fd_server_options(int fd);
/**
 * @brief Creates a raw socket file descriptor.
 * @return A socket FD on success, or an IoError.
 */
Result<int> create_raw_socket(int family);

/**
 * @brief Turns an ip/hostname port into a socket address.
 *
 * @note This method performs a DNS resolution for hostnames.
 * At this time the resolution is blocking so not ideal in a single worker context.
 * So we recommend using IP address directly if possible, until and async dns resolution
 * mechanism is built.
 *
 * @param ip_address ip or hostname
 * @param port port
 * @return
 */
Result<SocketAddress> resolve_address(std::string_view ip_address, uint16_t port);
}  // namespace kio::net

#endif  // KIO_NET_H
