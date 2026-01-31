//
// Created by Yao ACHI on 29/01/2026.
//

#include "aio/io.hpp"

#include "aio/net.hpp"
#include <arpa/inet.h>
namespace aio
{

//=============================================
// Socket address
//===============================================

net::SocketAddress net::SocketAddress::V4(uint16_t port, const char* ip)
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

net::SocketAddress net::SocketAddress::V6(uint16_t port, const char* ip)
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

[[nodiscard]] std::optional<std::string> net::SocketAddress::GetIp() const
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

[[nodiscard]] std::optional<uint16_t> net::SocketAddress::GetPort() const
{
    if (addr.ss_family == AF_INET)
    {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        return ntohs(in->sin_port);
    }
    else if (addr.ss_family == AF_INET6)
    {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        return ntohs(in6->sin6_port);
    }
    return std::nullopt;
}

//=============================================
// Io Buffer
//===============================================

}  // namespace aio