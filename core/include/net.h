//
// Created by Yao ACHI on 06/10/2025.
//

#ifndef KIO_NET_H
#define KIO_NET_H
#include <arpa/inet.h>
#include <cstdint>
#include <expected>
#include <string_view>

#include "core/include/errors.h"

namespace kio::net
{
    struct SocketAddress
    {
        sockaddr_storage addr{};
        socklen_t addrlen{};
        int family{};
    };

    /**
     * Creates a TCP socket and start listening on it. This socket is set as reuse and non-blocking
     * @param ip_address
     * @param port
     * @param backlog
     * @return
     */
    std::expected<int, Error> create_tcp_socket(std::string_view ip_address, std::uint16_t port, int backlog);
    std::expected<void, Error> listen_on_sock(int fd, const SocketAddress& addr, int backlog);
    // set so_reuse on sockets
    std::expected<void, Error> set_fd_server_options(int fd);
    /**
     * @brief Creates a raw socket file descriptor.
     * @return A socket FD on success, or an IoError.
     */
    std::expected<int, Error> create_raw_socket(int family);
    /**
     * Turns an ip/hostname port into a socket address
     * @param ip_address ip or hostname
     * @param port port
     * @return
     */
    std::expected<SocketAddress, Error> parse_address(std::string_view ip_address, uint16_t port);
}  // namespace kio::net

#endif  // KIO_NET_H
