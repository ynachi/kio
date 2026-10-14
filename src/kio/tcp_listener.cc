#include "kio/tcp_listener.hpp"

#include <sys/socket.h>

namespace kio
{
    Result<io::UniqueFd> TcpListener::bind(const SocketAddress& addr, const int backlog)
    {
        // Create the raw socket
        const int raw_fd = ::socket(addr.addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (raw_fd < 0)
        {
            return Error::fail_errno(errno, "socket");
        }

        // Immediately wrap in the RAII fd primitive
        io::UniqueFd sock{raw_fd};

        // Reuse the variadic set_options!
        // Note: TCP_NODELAY set on a listening socket is inherited by accepted clients in Linux.
        if (auto r = sock.set_options(SockOpt::ReuseAddr{true}, SockOpt::ReusePort{true},
                                      SockOpt::NoDelay{true}, SockOpt::NonBlocking{true});
            !r)
        {
            return std::unexpected(r.error());
        }

        // Explicit bind
        if (::bind(sock.get(), addr.Get(), addr.addrlen) < 0)
        {
            return Error::fail_errno(errno, "bind");
        }

        // Start listening
        if (::listen(sock.get(), backlog) < 0)
        {
            return Error::fail_errno(errno, "listen");
        }

        return sock;
    }

    Result<io::UniqueFd> TcpListener::bind(const uint16_t port, const char* ip, const int backlog)
    {
        const bool v6 = ip != nullptr && std::string_view(ip).find(':') != std::string_view::npos;
        auto addr = v6 ? SocketAddress::V6(port, ip) : SocketAddress::V4(port, ip);
        if (!addr)
        {
            return std::unexpected(addr.error());
        }

        return bind(*addr, backlog);
    }
} // namespace kio
