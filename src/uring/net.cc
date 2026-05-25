#include "uring/net.hpp"

#include <cstring>

#include <netdb.h>

#include <arpa/inet.h>

namespace URing
{
SocketAddress SocketAddress::V4(const uint16_t port, const char* ip)
{
    SocketAddress sa;
    auto* in = reinterpret_cast<sockaddr_in*>(&sa.addr);
    in->sin_family = AF_INET;
    in->sin_port = htons(port);
    if (ip && *ip)
    {
        inet_pton(AF_INET, ip, &in->sin_addr);
    }
    else
    {
        in->sin_addr.s_addr = INADDR_ANY;
    }
    sa.addrlen = sizeof(sockaddr_in);
    return sa;
}

SocketAddress SocketAddress::V6(const uint16_t port, const char* ip)
{
    SocketAddress sa;
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&sa.addr);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(port);
    if (ip && *ip)
    {
        inet_pton(AF_INET6, ip, &in6->sin6_addr);
    }
    else
    {
        in6->sin6_addr = in6addr_any;
    }
    sa.addrlen = sizeof(sockaddr_in6);
    return sa;
}

[[nodiscard]] std::optional<std::string> SocketAddress::GetIp() const
{
    char buffer[INET6_ADDRSTRLEN];
    if (addr.ss_family == AF_INET)
    {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        if (inet_ntop(AF_INET, &in->sin_addr, buffer, sizeof(buffer)))
        {
            return std::string(buffer);
        }
    }
    else if (addr.ss_family == AF_INET6)
    {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (inet_ntop(AF_INET6, &in6->sin6_addr, buffer, sizeof(buffer)))
        {
            return std::string(buffer);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<uint16_t> SocketAddress::GetPort() const
{
    if (addr.ss_family == AF_INET)
    {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        return ntohs(in->sin_port);
    }
    if (addr.ss_family == AF_INET6)
    {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        return ntohs(in6->sin6_port);
    }
    return std::nullopt;
}

Result<SocketAddress> ResolveIp(const std::string_view host, const uint16_t port)
{
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string service = std::to_string(port);
    const std::string hostname(host);

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
}  // namespace URing