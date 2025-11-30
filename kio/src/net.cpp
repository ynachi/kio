//
// Created by Yao ACHI on 06/10/2025.
//

#include "kio/include/net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <string_view>

#include "kio/include/async_logger.h"


namespace kio::net {
    Result<SocketAddress> parse_address(std::string_view ip_address, const uint16_t port) {
        SocketAddress result;

        // Try IPv4 first
        auto *addr4 = reinterpret_cast<sockaddr_in *>(&result.addr);
        if (inet_pton(AF_INET, ip_address.data(), &addr4->sin_addr) == 1) {
            result.family = AF_INET;
            addr4->sin_family = AF_INET;
            addr4->sin_port = htons(port);
            result.addrlen = sizeof(sockaddr_in);
        }
        // Try IPv6
        else {
            auto *addr6 = reinterpret_cast<sockaddr_in6 *>(&result.addr);
            if (inet_pton(AF_INET6, ip_address.data(), &addr6->sin6_addr) == 1) {
                result.family = AF_INET6;
                addr6->sin6_family = AF_INET6;
                addr6->sin6_port = htons(port);
                result.addrlen = sizeof(sockaddr_in6);
            } else {
                ALOG_ERROR("invalid IP address: {}", ip_address);
                return std::unexpected(Error::from_errno(EINVAL));
            }
        }

        ALOG_DEBUG("created IP:Port endpoint: {}:{}", ip_address, port);
        return result;
    }

    Result<int> create_raw_socket(const int family) {
        const int server_fd = ::socket(family, SOCK_STREAM, 0);
        if (server_fd < 0) {
            const int err = errno;
            ALOG_ERROR("socket failed: {}", strerror(err));
            return std::unexpected(Error::from_errno(err));
        }
        return server_fd;
    }

    Result<int> create_tcp_socket(std::string_view ip_address, const uint16_t port, const int backlog) {
        auto socket_addr = parse_address(ip_address, port);
        if (!socket_addr) {
            return std::unexpected(socket_addr.error());
        }
        ALOG_DEBUG("successfully created IP:Port endpoint: {}:{}", ip_address, port);

        // Create socket
        auto server_fd = create_raw_socket(socket_addr->family);
        if (!server_fd) {
            return std::unexpected(server_fd.error());
        }
        ALOG_DEBUG("created socket fd: {}", server_fd.value());

        if (auto res = set_fd_server_options(*server_fd); !res) {
            ::close(*server_fd);
            return std::unexpected(res.error());
        }
        ALOG_DEBUG("successfully set socket options");

        if (auto res = listen_on_sock(*server_fd, *socket_addr, backlog); !res) {
            ::close(*server_fd);
            return std::unexpected(res.error());
        }
        ALOG_DEBUG("successfully listened on socket fd: {}", server_fd.value());

        return server_fd;
    }

    Result<void> listen_on_sock(const int fd, const SocketAddress &addr, const int backlog) {
        // Bind
        if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr.addr), addr.addrlen)) {
            return std::unexpected(Error::from_errno(errno));
        }

        // Set non-blocking
        if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            return std::unexpected(Error::from_errno(errno));
        }

        // now listen
        if (::listen(fd, backlog) < 0) {
            return std::unexpected(Error::from_errno(errno));
        }

        return {};
    }

    Result<void> set_fd_server_options(const int fd) {
        constexpr int option = 1;

        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) < 0) {
            return std::unexpected(Error::from_errno(errno));
        }

#ifdef SO_REUSEPORT
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option)) < 0) {
            return std::unexpected(Error::from_errno(errno));
        }
#endif

        return {};
    }
} // namespace kio::net
