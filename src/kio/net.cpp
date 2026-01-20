#include "kio/net.hpp"

#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <netinet/tcp.h>

namespace kio::net
{

// ----------------------------------------------------------------------------
// Socket Implementation
// ----------------------------------------------------------------------------

void Socket::close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

Result<void> Socket::set_non_blocking() const
{
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags == -1)
        return error_from_errno(errno);
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1)
        return error_from_errno(errno);
    return {};
}

Result<> Socket::set_reuse_addr(bool enable) const
{
    int opt = enable ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        return error_from_errno(errno);
    return {};
}

Result<> Socket::set_reuse_port(bool enable) const
{
    int opt = enable ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        return error_from_errno(errno);
    return {};
}

Result<> Socket::set_nodelay(bool enable) const
{
    int opt = enable ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0)
        return error_from_errno(errno);
    return {};
}

Result<> Socket::set_send_buffer(int size) const
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
        return error_from_errno(errno);
    return {};
}

Result<> Socket::set_recv_buffer(int size) const
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)
        return error_from_errno(errno);
    return {};
}

// ----------------------------------------------------------------------------
// SocketAddress Implementation
// ----------------------------------------------------------------------------

Result<SocketAddress> SocketAddress::resolve(std::string_view host, uint16_t port)
{
    addrinfo hints{}, *res = nullptr;
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

// ----------------------------------------------------------------------------
// TcpListener Implementation
// ----------------------------------------------------------------------------

Result<Socket> TcpListener::bind(const SocketAddress& addr, int backlog)
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

Result<Socket> TcpListener::bind(uint16_t port)
{
    return bind(SocketAddress::v4(port));
}

}  // namespace kio::net