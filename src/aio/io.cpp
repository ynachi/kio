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

void IoBuffer::Consume(size_t n)
{
    if (n > ReadableBytes())
    {
        throw std::out_of_range("IoBuffer::Consume() beyond available data");
    }
    read_offset_ += n;
}

void IoBuffer::EnsureWritableBytes(size_t additional)
{
    if (WritableBytes() >= additional)
    {
        return;
    }

    const size_t kDataLen = ReadableBytes();
    const size_t kAvailableTotal = data_.capacity();

    // Strategy 1: Compact in place if it fits
    if (kAvailableTotal >= kDataLen + additional)
    {
        // Threshold: only compact if significant waste
        if (read_offset_ >= kAutoCompactionThresholdBytes || read_offset_ >= data_.size() / 4)
        {
            Compact();
        }

        if (data_.size() < write_offset_ + additional)
        {
            data_.resize(write_offset_ + additional);
        }
        return;
    }

    // Strategy 2: Reallocate, copy ONLY live data
    const size_t kNewCapacity = std::max(kAvailableTotal * 2, kDataLen + additional);
    std::vector<char> new_buffer;
    new_buffer.reserve(kNewCapacity);
    new_buffer.resize(kNewCapacity);

    if (kDataLen > 0)
    {
        std::memcpy(new_buffer.data(), data_.data() + read_offset_, kDataLen);
    }

    data_ = std::move(new_buffer);
    read_offset_ = 0;
    write_offset_ = kDataLen;
}

[[nodiscard]] std::span<const char> IoBuffer::Peek(size_t n) const
{
    if (n > ReadableBytes())
    {
        throw std::out_of_range("IoBuffer::Peek() beyond available data");
    }
    return {data_.data() + read_offset_, n};
}

void IoBuffer::Commit(size_t n)
{
    if (write_offset_ + n > data_.size())
    {
        throw std::out_of_range("IoBuffer::Commit() beyond buffer size");
    }
    write_offset_ += n;
}

void IoBuffer::Append(std::span<const char> data)
{
    EnsureWritableBytes(data.size());
    std::memcpy(data_.data() + write_offset_, data.data(), data.size());
    write_offset_ += data.size();
}

void IoBuffer::Compact()
{
    if (read_offset_ == 0)
    {
        return;
    }

    const size_t live_bytes = ReadableBytes();

    if (live_bytes > 0)
    {
        std::memmove(data_.data(), data_.data() + read_offset_, live_bytes);
    }

    read_offset_ = 0;
    write_offset_ = live_bytes;
}
}  // namespace aio