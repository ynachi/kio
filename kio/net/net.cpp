//
// Created by Yao ACHI on 06/10/2025.
//

#include "net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <string_view>

#include "kio/core/async_logger.h"


namespace kio::net
{
    Result<SocketAddress> resolve_address(std::string_view host, const uint16_t port)
    {
        SocketAddress result;
        const std::string host_str(host);

        // ---- Try IPv4 ----
        auto* addr4 = reinterpret_cast<sockaddr_in*>(&result.addr);
        if (inet_pton(AF_INET, host_str.c_str(), &addr4->sin_addr) == 1)
        {
            result.family = AF_INET;
            addr4->sin_family = AF_INET;
            addr4->sin_port = htons(port);
            result.addrlen = sizeof(sockaddr_in);
            return result;
        }

        // ---- Try IPv6 ----
        auto* addr6 = reinterpret_cast<sockaddr_in6*>(&result.addr);
        if (inet_pton(AF_INET6, host_str.c_str(), &addr6->sin6_addr) == 1)
        {
            result.family = AF_INET6;
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = htons(port);
            result.addrlen = sizeof(sockaddr_in6);
            return result;
        }

        // ---- DNS lookup ----
        // TODO: this is synchronous and can block the whole worker thread.
        // Implement async API
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* res = nullptr;
        std::string port_str = std::to_string(port);

        if (getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &res) != 0)
        {
            ALOG_ERROR("DNS resolution failed for {}", host);
            return std::unexpected(Error{ErrorCategory::Network, ENOENT});
        }

        result.family = res->ai_family;
        result.addrlen = res->ai_addrlen;
        std::memcpy(&result.addr, res->ai_addr, res->ai_addrlen);

        freeaddrinfo(res);
        return result;
    }

    Result<int> create_tcp_fd(const int family)
    {
        const int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            const int err = errno;
            ALOG_ERROR("Failed to create socket: {}", strerror(err));
            return std::unexpected(Error::from_errno(err));
        }
        return fd;
    }

    // set common option for tcp
    Result<void> set_tcp_fd_options(const int fd)
    {
        constexpr int opt = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
        return {};
    }

    Result<SocketAddress> resolve_endpoint(std::string_view host, uint16_t port)
    {
        SocketAddress out;
        out.port = port;

        // Try IPv4
        sockaddr_in v4{};
        if (inet_pton(AF_INET, host.data(), &v4.sin_addr) == 1)
        {
            v4.sin_family = AF_INET;
            v4.sin_port = htons(port);

            out.addrlen = sizeof(sockaddr_in);
            std::memcpy(&out.addr, &v4, out.addrlen);
            out.populate_from_storage();
            return out;
        }

        // Try IPv6
        sockaddr_in6 v6{};
        if (inet_pton(AF_INET6, host.data(), &v6.sin6_addr) == 1)
        {
            v6.sin6_family = AF_INET6;
            v6.sin6_port = htons(port);

            out.addrlen = sizeof(sockaddr_in6);
            std::memcpy(&out.addr, &v6, out.addrlen);
            out.populate_from_storage();
            return out;
        }

        // DNS lookup
        // TODO, make dns lookup async
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* res;
        if (getaddrinfo(host.data(), nullptr, &hints, &res) != 0) return std::unexpected(Error::from_errno(ENOENT));

        // Take the first result
        std::memcpy(&out.addr, res->ai_addr, res->ai_addrlen);
        out.addrlen = res->ai_addrlen;

        freeaddrinfo(res);

        // Populate ip / port / family from addr
        out.populate_from_storage();

        // Now override the port (DNS does not include it)
        if (out.family == AF_INET)
            reinterpret_cast<sockaddr_in*>(&out.addr)->sin_port = htons(port);
        else
            reinterpret_cast<sockaddr_in6*>(&out.addr)->sin6_port = htons(port);

        out.port = port;

        return out;
    }


    [[deprecated("Deprecated: will be removed, use create_tcp_socket instead")]]
    Result<int> create_raw_socket(const int family)
    {
        const int server_fd = ::socket(family, SOCK_STREAM, 0);
        if (server_fd < 0)
        {
            const int err = errno;
            ALOG_ERROR("socket failed: {}", strerror(err));
            return std::unexpected(Error::from_errno(err));
        }
        return server_fd;
    }

    // deprecated, Do not use
    [[deprecated("Deprecated: will be removed")]]
    Result<int> create_tcp_server_socket(std::string_view ip_address, const uint16_t port, const int backlog)
    {
        auto socket_addr = resolve_address(ip_address, port);
        if (!socket_addr)
        {
            return std::unexpected(socket_addr.error());
        }
        ALOG_DEBUG("successfully created IP:Port endpoint: {}:{}", ip_address, port);

        // Create socket
        auto server_fd = create_raw_socket(socket_addr->family);
        if (!server_fd)
        {
            return std::unexpected(server_fd.error());
        }
        ALOG_DEBUG("created socket fd: {}", server_fd.value());

        if (auto res = set_fd_server_options(*server_fd); !res)
        {
            ::close(*server_fd);
            return std::unexpected(res.error());
        }
        ALOG_DEBUG("successfully set socket options");

        if (auto res = listen_on_sock(*server_fd, *socket_addr, backlog); !res)
        {
            ::close(*server_fd);
            return std::unexpected(res.error());
        }
        ALOG_DEBUG("successfully listened on socket fd: {}", server_fd.value());

        return server_fd;
    }

    Result<void> listen_on_sock(const int fd, const SocketAddress& addr, const int backlog)
    {
        // Bind
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr.addr), addr.addrlen))
        {
            return std::unexpected(Error::from_errno(errno));
        }

        // Set non-blocking
        if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
        {
            return std::unexpected(Error::from_errno(errno));
        }

        // now listen
        if (::listen(fd, backlog) < 0)
        {
            return std::unexpected(Error::from_errno(errno));
        }

        return {};
    }

    Result<void> set_fd_server_options(const int fd)
    {
        constexpr int option = 1;

        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) < 0)
        {
            return std::unexpected(Error::from_errno(errno));
        }

#ifdef SO_REUSEPORT
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option)) < 0)
        {
            return std::unexpected(Error::from_errno(errno));
        }
#endif

        return {};
    }
}  // namespace kio::net
