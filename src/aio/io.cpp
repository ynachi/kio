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
std::span<const std::byte> IoBuffer::ReadableSpan() const
{
    if (IsEmpty())
    {
        return {};
    }

    const size_t read_idx = read_offset_ & mask_;
    const size_t write_idx = write_offset_ & mask_;

    if (read_idx < write_idx)
    {
        // Normal case: [read_idx, write_idx)
        return {data_.data() + read_idx, write_idx - read_idx};
    }

    // Wrapped case: [read_idx, end)
    return {data_.data() + read_idx, data_.size() - read_idx};
}

std::pair<std::span<const std::byte>, std::span<const std::byte>> IoBuffer::ReadableSpans() const
{
    if (IsEmpty())
    {
        return {{}, {}};
    }

    const size_t read_idx = read_offset_ & mask_;
    const size_t write_idx = write_offset_ & mask_;

    if (read_idx < write_idx)
    {
        // Normal case: single contiguous segment
        return {
            {data_.data() + read_idx, write_idx - read_idx},
            {}
        };
    }
    // Wrapped case: two segments
    return {
        {data_.data() + read_idx, data_.size() - read_idx}, // Tail
        {data_.data(),            write_idx              }  // Head
    };
}

[[nodiscard]] std::vector<iovec> IoBuffer::ReadableIovecs() const
{
    auto [s1, s2] = ReadableSpans();
    std::vector<iovec> vecs;
    vecs.reserve(s2.empty() ? 1 : 2);
    if (!s1.empty())
    {
        vecs.push_back({(void*)s1.data(), s1.size()});
    }
    if (!s2.empty())
    {
        vecs.push_back({(void*)s2.data(), s2.size()});
    }
    return vecs;
}

size_t IoBuffer::Consume(const size_t n) noexcept
{
    const auto real_consume = std::min(n, ReadableBytes());
    read_offset_ += real_consume;

    // Optional: Reset positions when the buffer is empty to avoid overflow long-term
    // (though uint64_t would take centuries to overflow)
    if (read_offset_ == write_offset_)
    {
        read_offset_ = 0;
        write_offset_ = 0;
    }

    return real_consume;
}

[[nodiscard]] std::span<std::byte> IoBuffer::WritableSpan() noexcept
{
    if (IsFull())
    {
        return {};
    }

    const size_t read_idx = read_offset_ & mask_;
    const size_t write_idx = write_offset_ & mask_;

    if (write_idx >= read_idx)
    {
        // Normal case: can write to end of buffer
        const size_t available_to_end = data_.size() - write_idx;
        const size_t total_writable = WritableBytes();

        // We can write until end of buffer OR until we hit the read pointer (virtually)
        // Since read_idx <= write_idx in this branch, we are bounded by buffer size or capacity.
        return {(data_.data() + write_idx), std::min(available_to_end, total_writable)};
    }
    // Wrapped case: can write [write_idx, read_idx)
    return {(data_.data() + write_idx), read_idx - write_idx};
}

[[nodiscard]] std::pair<std::span<std::byte>, std::span<std::byte>> IoBuffer::WritableSpans()
{
    if (IsFull())
    {
        return {{}, {}};
    }

    const size_t read_idx = read_offset_ & mask_;
    const size_t write_idx = write_offset_ & mask_;

    if (write_idx >= read_idx)
    {
        // Normal case: may have space at end and beginning
        const size_t space_at_end = data_.size() - write_idx;
        const size_t total_writable = WritableBytes();

        // First chunk: up to end of buffer
        size_t len1 = std::min(space_at_end, total_writable);
        std::span<std::byte> s1{reinterpret_cast<std::byte*>(data_.data() + write_idx), len1};

        // Second chunk: remainder at the beginning
        std::span<std::byte> s2{};
        if (len1 < total_writable)
        {
            size_t len2 = total_writable - len1;
            s2 = {(data_.data()), len2};
        }
        return {s1, s2};
    }

    // Wrapped case: single segment [write_idx, read_idx)
    return {
        {(data_.data() + write_idx), read_idx - write_idx},
        {}
    };
}

[[nodiscard]] std::vector<iovec> IoBuffer::WritableIovecs() noexcept
{
    auto [s1, s2] = WritableSpans();
    std::vector<iovec> vecs;
    vecs.reserve(s2.empty() ? 1 : 2);
    if (!s1.empty())
    {
        vecs.push_back({(void*)s1.data(), s1.size()});
    }
    if (!s2.empty())
    {
        vecs.push_back({(void*)s2.data(), s2.size()});
    }
    return vecs;
}

[[nodiscard]] size_t IoBuffer::Commit(size_t n) noexcept
{
    auto actual_commit = std::min(n, WritableBytes());
    write_offset_ += actual_commit;
    return actual_commit;
}

void IoBuffer::EnsureWritableBytes(size_t n)
{
    if (WritableBytes() >= n)
    {
        return;
    }

    // Need to grow buffer
    const size_t current_data_len = ReadableBytes();
    const size_t new_capacity = std::bit_ceil(current_data_len + n);

    // Linearize to new buffer
    std::vector<std::byte> new_data(new_capacity);

    auto [span1, span2] = ReadableSpans();
    size_t copied = 0;

    if (!span1.empty())
    {
        std::memcpy(new_data.data(), span1.data(), span1.size());
        copied += span1.size();
    }

    if (!span2.empty())
    {
        std::memcpy(new_data.data() + copied, span2.data(), span2.size());
        copied += span2.size();
    }

    data_ = std::move(new_data);
    mask_ = new_capacity - 1;
    read_offset_ = 0;
    write_offset_ = current_data_len;
}

void IoBuffer::Append(std::span<const std::byte> data)
{
    EnsureWritableBytes(data.size());

    // Write in up to two segments if wrapped
    size_t written = 0;
    auto [span1, span2] = WritableSpans();

    if (!span1.empty())
    {
        const size_t to_write = std::min(span1.size(), data.size());
        std::memcpy(span1.data(), data.data(), to_write);
        written += to_write;
    }

    if (written < data.size() && !span2.empty())
    {
        const size_t to_write = data.size() - written;
        std::memcpy(span2.data(), data.data() + written, to_write);
        written += to_write;
    }

    (void)Commit(written);
}
}  // namespace aio