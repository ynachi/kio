//
// Created by Yao ACHI on 10/12/2025.
//

#ifndef KIO_LISTENER_H
#define KIO_LISTENER_H
#include "context.h"
#include "kio/net/socket.h"
#include "stream.h"

namespace kio::tls
{
struct ListenerConfig
{
    constexpr static uint16_t kDefaultPort = 443;
    constexpr static int kDefaultBacklog = 128;

    uint16_t port{kDefaultPort};
    std::string bind_address{"0.0.0.0"};
    int backlog{kDefaultBacklog};
    bool reuse_port{true};
    bool reuse_addr{true};
    bool tcp_nodelay{true};
};

class TlsListener
{
    TlsListener(io::Worker& w, net::Socket s, TlsContext& ctx) : worker_(w), listen_sock_(std::move(s)), ctx_(ctx) {}

    io::Worker& worker_;
    net::Socket listen_sock_;
    TlsContext& ctx_;

public:
    static Result<TlsListener> Bind(io::Worker& worker, const ListenerConfig& config, TlsContext& ctx);

    [[nodiscard]] Task<Result<TlsStream>> Accept() const;
};

class TlsConnector
{
public:
    TlsConnector(io::Worker& worker, TlsContext& ctx) : worker_(worker), ctx_(ctx) {}

    Task<Result<TlsStream>> Connect(std::string_view hostname, uint16_t port);

private:
    io::Worker& worker_;
    TlsContext& ctx_;
};

}  // namespace kio::tls

#endif  // KIO_LISTENER_H
