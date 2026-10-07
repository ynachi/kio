#include "uring/net.hpp"

#include <cerrno>
#include <cstring>

#include <netdb.h>

#include <arpa/inet.h>

namespace kio
{
    Result<SocketAddress> SocketAddress::V4(const uint16_t port, const char* ip)
    {
        SocketAddress sa;
        auto* in = reinterpret_cast<sockaddr_in*>(&sa.addr);
        in->sin_family = AF_INET;
        in->sin_port = htons(port);
        if (ip && *ip)
        {
            // A malformed literal must not fall through to INADDR_ANY: a typo would
            // silently widen a bind to every interface.
            if (inet_pton(AF_INET, ip, &in->sin_addr) != 1)
            {
                return Error::fail_errc(std::errc::invalid_argument, "inet_pton(AF_INET)");
            }
        }
        else
        {
            in->sin_addr.s_addr = INADDR_ANY;
        }
        sa.addrlen = sizeof(sockaddr_in);
        return sa;
    }

    Result<SocketAddress> SocketAddress::V6(const uint16_t port, const char* ip)
    {
        SocketAddress sa;
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&sa.addr);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        if (ip && *ip)
        {
            if (inet_pton(AF_INET6, ip, &in6->sin6_addr) != 1)
            {
                return Error::fail_errc(std::errc::invalid_argument, "inet_pton(AF_INET6)");
            }
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
            // getaddrinfo returns EAI_* codes, not errno; map the ones with a clear
            // errc equivalent and fall back to address_not_available.
            switch (rc)
            {
            case EAI_SYSTEM:
                return Error::fail_errno(errno, "getaddrinfo");
            case EAI_AGAIN:
                return Error::fail_errc(std::errc::resource_unavailable_try_again, "getaddrinfo");
            case EAI_MEMORY:
                return Error::fail_errc(std::errc::not_enough_memory, "getaddrinfo");
            default:
                return Error::fail_errc(std::errc::address_not_available, "getaddrinfo");
            }
        }

        if (res == nullptr || res->ai_addrlen > sizeof(sockaddr_storage))
        {
            if (res != nullptr)
            {
                freeaddrinfo(res);
            }
            return Error::fail_errc(std::errc::address_not_available, "getaddrinfo");
        }

        SocketAddress out;
        std::memcpy(&out.addr, res->ai_addr, res->ai_addrlen);
        out.addrlen = res->ai_addrlen;
        freeaddrinfo(res);
        return out;
    }
} // namespace URing
