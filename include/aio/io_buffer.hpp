#pragma once

#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <vector>

namespace aio
{
/**
 * @brief A growable buffer optimized for reading data in chunks and consuming it.
 *
 * Features:
 * - Lazy Compaction: Only moves memory when strictly necessary.
 * - Smart Growth: Copies only live data when resizing.
 *
 * Memory layout:
 * [consumed data | readable data | writable space]
 *                ^read_pos       ^write_pos      ^capacity
 *
 * Example usage:
 * ```cpp
 * BytesMut buf;
 * buf.reserve(4096);
 *
 * // Write data
 * auto writable = buf.writable_span();
 * ssize_t n = read(fd, writable.data(), writable.size());
 * buf.commit_write(n);
 *
 * // Read data
 * while (buf.remaining() >= header_size) {
 *     auto data = buf.readable_span();
 *     auto [entry, size] = parse_entry(data);
 *     buf.advance(size);  // consume
 * }
 *
 * // Reclaim space when read_pos gets large
 * if (buf.should_compact()) {
 *     buf.compact();
 * }
 * ```
 */
class IoBuffer
{
    std::vector<char> data_;
    size_t read_offset_ = 0;
    size_t write_offset_ = 0;
    static constexpr size_t kAutoCompactionThresholdBytes = 1024;

public:
    IoBuffer() = default;

    explicit IoBuffer(const size_t cap)
    {
        data_.reserve(cap);
        data_.resize(cap);
    }

    /**
     * @brief Ensures at least `additional` bytes of writable space.
     * May trigger reallocation.
     */
    void EnsureWritableBytes(size_t additional);

    [[nodiscard]] size_t Capacity() const { return data_.capacity(); }

    /**
     * @brief Returns a span of readable data (not yet consumed)
     */
    [[nodiscard]] std::span<const char> ReadableSpan() const
    {
        return {data_.data() + read_offset_, write_offset_ - read_offset_};
    }

    /**
     * @brief Number of bytes available to read
     */
    [[nodiscard]] size_t ReadableBytes() const { return write_offset_ - read_offset_; }

    /**
     * @brief Consumes `n` bytes from the read position
     * @param n Number of bytes to consume
     */
    void Consume(size_t n)
    {
        if (n > ReadableBytes())
        {
            throw std::out_of_range("IoBuffer::Consume() beyond available data");
        }
        read_offset_ += n;
    }

    [[nodiscard]] std::span<const char> Peek(size_t n) const
    {
        if (n > ReadableBytes())
        {
            throw std::out_of_range("IoBuffer::Peek() beyond available data");
        }
        return {data_.data() + read_offset_, n};
    }

    [[nodiscard]] std::span<char> WritableSpan()
    {
        return {data_.data() + write_offset_, data_.size() - write_offset_};
    }

    [[nodiscard]] size_t WritableBytes() const { return data_.size() - write_offset_; }

    // Call after writing to WritableSpan().
    void Commit(size_t n)
    {
        if (write_offset_ + n > data_.size())
        {
            throw std::out_of_range("IoBuffer::Commit() beyond buffer size");
        }
        write_offset_ += n;
    }

    void Append(std::span<const char> data)
    {
        EnsureWritableBytes(data.size());
        std::memcpy(data_.data() + write_offset_, data.data(), data.size());
        write_offset_ += data.size();
    }

    void Compact()
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

    [[nodiscard]] bool ShouldCompact() const { return read_offset_ >= data_.size() / 2; }

    void Clear()
    {
        read_offset_ = 0;
        write_offset_ = 0;
    }

    [[nodiscard]] bool Empty() const { return read_offset_ == write_offset_; }

    [[nodiscard]] size_t ConsumedBytes() const { return read_offset_; }
};
}  // namespace aio
