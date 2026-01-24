#include "aio/net.hpp"

#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <netinet/tcp.h>

namespace aio::net
{

// ----------------------------------------------------------------------------
// Socket Implementation
// ----------------------------------------------------------------------------

void Socket::Close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

Result<void> Socket::SetNonBlocking() const
{
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags == -1)
        return ErrorFromErrno(errno);
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1)
        return ErrorFromErrno(errno);
    return {};
}

Result<> Socket::SetReuseAddr(bool enable) const
{
    int opt = enable ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        return ErrorFromErrno(errno);
    return {};
}

Result<> Socket::SetReusePort(bool enable) const
{
    int opt = enable ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
        return ErrorFromErrno(errno);
    return {};
}

Result<> Socket::SetNodelay(bool enable) const
{
    int opt = enable ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0)
        return ErrorFromErrno(errno);
    return {};
}

Result<> Socket::SetSendBuffer(int size) const
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
        return ErrorFromErrno(errno);
    return {};
}

Result<> Socket::SetRecvBuffer(int size) const
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)
        return ErrorFromErrno(errno);
    return {};
}

// ----------------------------------------------------------------------------
// SocketAddress Implementation
// ----------------------------------------------------------------------------

Result<SocketAddress> Resolve(std::string_view host, uint16_t port)
{
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string service = std::to_string(port);
    const std::string hostname(host);  // getaddrinfo needs null-terminated

    if (const int rc = getaddrinfo(hostname.c_str(), service.c_str(), &hints, &res); rc != 0)
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

Result<Socket> TcpListener::Bind(const SocketAddress& addr, int backlog)
{
    int fd = ::socket(addr.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return ErrorFromErrno(errno);
    }

    Socket sock(fd);

    // Standard high-perf defaults
    if (auto r = sock.SetReuseAddr(); !r)
    {
        return std::unexpected(r.error());
    }
    if (auto r = sock.SetReusePort(); !r)
    {
        return std::unexpected(r.error());
    }

    // Explicit bind
    if (::bind(fd, addr.Get(), addr.addrlen) < 0)
    {
        return ErrorFromErrno(errno);
    }

    if (::listen(fd, backlog) < 0)
    {
        return ErrorFromErrno(errno);
    }

    return sock;
}

Result<Socket> TcpListener::Bind(uint16_t port)
{
    return Bind(SocketAddress::V4(port));
}

}  // namespace aio::net