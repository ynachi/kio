//
// Created by Yao ACHI on 23/11/2025.
//

#ifndef KIO_BYTES_MUT_H
#define KIO_BYTES_MUT_H
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <vector>

namespace kio
{
    /**
     * @brief A growable buffer optimized for reading data in chunks and consuming it.
     *
     * Inspired by Rust's BytesMut, this provides:
     * - Zero-copy reads via advance() (no memory until compact())
     * - Separate read and write regions
     * - Efficient growth with reserve()
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
    class BytesMut
    {
    public:
        BytesMut() = default;
        explicit BytesMut(const size_t initial_capacity) { buffer_.reserve(initial_capacity); }

        /**
         * @brief Ensures at least `additional` bytes of writable space.
         * May trigger compaction or reallocation.
         */
        void reserve(size_t additional)
        {
            if (writable() >= additional) return;

            // Try compacting first if there's significant consumed space
            if (read_pos_ >= buffer_.size() / kCompressionFactor && read_pos_ > 0)
            {
                compact();
                if (writable() >= additional) return;
            }

            // Need to grow the buffer
            const size_t needed_capacity = write_pos_ + additional;
            buffer_.reserve(needed_capacity);
            buffer_.resize(needed_capacity);
        }

        /**
         * @brief Returns current capacity
         */
        [[nodiscard]] size_t capacity() const { return buffer_.capacity(); }

        /**
         * @brief Returns a span of readable data (not yet consumed)
         */
        [[nodiscard]] std::span<const char> readable_span() const { return {buffer_.data() + read_pos_, write_pos_ - read_pos_}; }

        /**
         * @brief Number of bytes available to read
         */
        [[nodiscard]] size_t remaining() const { return write_pos_ - read_pos_; }

        /**
         * @brief Consumes `n` bytes from the read position
         * @param n Number of bytes to consume
         */
        void advance(const size_t n)
        {
            if (n > remaining())
            {
                throw std::out_of_range("BytesMut::advance() beyond available data");
            }
            read_pos_ += n;
        }

        /**
         * @brief Peek at bytes without consuming
         */
        [[nodiscard]] std::span<const char> peek(const size_t n) const
        {
            if (n > remaining())
            {
                throw std::out_of_range("BytesMut::peek() beyond available data");
            }
            return {buffer_.data() + read_pos_, n};
        }

        /**
         * @brief Returns a span of writable space
         */
        [[nodiscard]] std::span<char> writable_span() { return {buffer_.data() + write_pos_, buffer_.size() - write_pos_}; }

        /**
         * @brief Number of bytes available for writing
         */
        [[nodiscard]] size_t writable() const { return buffer_.size() - write_pos_; }

        /**
         * @brief Commits `n` bytes of written data
         * Call this after writing to writable_span()
         */
        void commit_write(const size_t n)
        {
            if (write_pos_ + n > buffer_.size())
            {
                throw std::out_of_range("BytesMut::commit_write() beyond buffer size");
            }
            write_pos_ += n;
        }

        /**
         * @brief Appends data to the buffer
         */
        void extend_from_slice(std::span<const char> data)
        {
            reserve(data.size());
            std::memcpy(buffer_.data() + write_pos_, data.data(), data.size());
            write_pos_ += data.size();
        }

        /**
         * @brief Moves unconsumed data to the front of the buffer
         */
        void compact()
        {
            if (read_pos_ == 0) return;

            const size_t remaining_bytes = remaining();

            if (remaining_bytes > 0)
            {
                std::memmove(buffer_.data(), buffer_.data() + read_pos_, remaining_bytes);
            }

            read_pos_ = 0;
            write_pos_ = remaining_bytes;
        }


        /**
         * @brief Checks if compaction is beneficial
         * Returns true if more than half the buffer is consumed
         */
        [[nodiscard]] bool should_compact() const { return read_pos_ >= buffer_.size() / 2; }

        /**
         * @brief Resets the buffer to an empty state.
         * It does not reallocate. It internally  moves cursors to the initial positions.
         */
        void clear()
        {
            read_pos_ = 0;
            write_pos_ = 0;
        }

        /**
         * @brief Returns true if no data is available to read
         */
        [[nodiscard]] bool is_empty() const { return read_pos_ == write_pos_; }

        /**
         * @brief Returns the number of consumed bytes (wasted space)
         */
        [[nodiscard]] size_t consumed() const { return read_pos_; }

        /**
         * @brief Debug info
         */
        std::string debug()
        {
            return std::format(
                    "BytesMut {{ read_pos: {}, write_pos: {}, capacity: {}, "
                    "remaining: {}, writable: {}, consumed: {} }}\n",
                    read_pos_, write_pos_, buffer_.capacity(), remaining(), writable(), consumed());
        }


    private:
        std::vector<char> buffer_;
        size_t read_pos_ = 0;
        size_t write_pos_ = 0;
        const size_t kCompressionFactor = 2;
    };
}  // namespace kio
#endif  // KIO_BYTES_MUT_H
