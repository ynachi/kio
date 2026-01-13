//
// Created by Yao ACHI on 10/12/2025.
//

#include "listener.h"

#include <netinet/tcp.h>

#include "kio/net/net.h"

using namespace kio::io;
using namespace kio::net;

namespace kio::tls
{
Result<TlsListener> TlsListener::Bind(Worker& worker, const ListenerConfig& config, TlsContext& ctx)
{
    auto fd_res = create_tcp_fd(AF_INET);
    if (!fd_res.has_value())
    {
        return std::unexpected(fd_res.error());
    }

    // use the socket guard
    auto socket = Socket(fd_res.value());

    // Socket options
    constexpr int opt = 1;
    if (config.reuse_addr)
    {
        if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        {
            return std::unexpected(Error::FromErrno(errno));
        }
    }

    if (config.reuse_port)
    {
        ::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    auto addr_res = resolve_address(config.bind_address, config.port);
    if (!addr_res.has_value())
    {
        return std::unexpected(addr_res.error());
    }

    // now bind and listen
    if (auto res = listen_on_sock(socket.get(), addr_res.value(), config.backlog); !res.has_value())
    {
        return std::unexpected(res.error());
    }
    ALOG_DEBUG("successfully listened on socket fd: {}", socket.get());

    return TlsListener(worker, std::move(socket), ctx);
}

Task<Result<TlsStream>> TlsListener::Accept() const
{
    SocketAddress peer_addr;

    const auto client_fd = KIO_TRY(co_await worker_.AsyncAccept(listen_sock_, peer_addr));

    Socket client(client_fd);
    constexpr int opt = 1;
    // TODO: use the config
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Create Stream using the Listener's context
    TlsStream stream(worker_, std::move(client), ctx_, TlsRole::kServer);
    KIO_TRY(co_await stream.AsyncHandshake());

    // extract client ip and port
    peer_addr.populate_from_storage();

    // set peer address
    stream.SetPeerAddr(std::move(peer_addr));

    co_return std::move(stream);
}

Task<Result<TlsStream>> TlsConnector::Connect(std::string_view hostname, uint16_t port)
{
    auto addr = KIO_TRY(resolve_address(hostname, port));
    ALOG_DEBUG("Performed address resolution");

    const auto fd = KIO_TRY(create_tcp_fd(addr.family));
    Socket socket(fd);
    ALOG_DEBUG("Created client FD {}", fd);

    KIO_TRY(set_tcp_fd_options(fd));
    ALOG_DEBUG("Socket options successfully set on FD {}", fd);

    KIO_TRY(co_await worker_.AsyncConnect(fd, addr.as_sockaddr(), addr.addrlen));
    ALOG_DEBUG("Connected to server {}:{}", hostname, port);

    TlsStream stream(worker_, std::move(socket), ctx_, TlsRole::kClient);
    KIO_TRY(co_await stream.AsyncHandshake(hostname));

    co_return std::move(stream);
}
}  // namespace kio::tls
