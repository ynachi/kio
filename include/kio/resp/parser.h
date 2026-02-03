#pragma once

#include "kio/core/core.hpp"
#include "kio/io.hpp"

#include <expected>
#include <span>
#include <string_view>

namespace kio::resp
{

struct ParserConfig
{
    size_t max_size = 512 * 1024 * 1024;  // 512 MB
    size_t max_aggregate_depth = 32;
    size_t initial_buffer = 64 * 1024;  // 64 KB
};

/**
 * @brief RESP Data Types mapped to their protocol byte indicators.
 */
enum class FrameType : uint8_t
{
    // Simple types (CRLF-terminated)
    kSimpleString = '+',
    kSimpleError = '-',
    kInteger = ':',
    kNull = '_',
    kBoolean = '#',
    kDouble = ',',
    kBigNumber = '(',

    // Bulk types (length-prefixed)
    kBulkString = '$',
    kBulkError = '!',
    kVerbatimString = '=',

    // Aggregates (count + children)
    kArray = '*',
    kMap = '%',
    kSet = '~',
    kPush = '>',
    kAttribute = '|',
};

struct FrameHeader
{
    FrameType type;
    const char* data;      // Points directly into IoBuffer
    size_t size;           // Total frame size including header & children
    size_t element_count;  // For aggregates: number of semantic elements
};

class Parser;

// Zero-allocation iterator for streaming aggregate children
class FrameIterator
{
public:
    explicit FrameIterator(const FrameHeader& parent, Parser& parser, size_t depth = 0);

    // Get the next child frame
    std::expected<FrameHeader, ParseError> Next();

    [[nodiscard]] bool HasNext() const { return remaining_ > 0; }
    [[nodiscard]] size_t Remaining() const { return remaining_; }

private:
    Parser& parser_;
    const char* current_;
    const char* end_;  // Safety bound
    size_t remaining_;
    size_t depth_;
};

class Parser
{
    friend class FrameIterator;

    IoBuffer buffer_;
    ParserConfig config_;

    // Internal: parse frame starting at a given position
    std::expected<FrameHeader, ParseError> ParseFrameInternal(std::span<const char> data, size_t depth);

public:
    explicit Parser(const ParserConfig& config) : buffer_(config.initial_buffer), config_(config) {}

    IoBuffer& Buffer() { return buffer_; }
    [[nodiscard]] const IoBuffer& Buffer() const { return buffer_; }

    [[nodiscard]] std::expected<FrameHeader, ParseError> NextFrame()
    {
        // Use the conversion helper from IoBuffer to get char span
        auto bytes = buffer_.ReadableSpan();
        return ParseFrameInternal(bytes, 0);
    }

    // Consume bytes after processing a frame
    void Consume(const size_t n) { buffer_.Consume(n); }
    void Consume(const FrameHeader& frame) { Consume(frame.size); }
};

/**
 * BufferReader: A non-owning utility to walk through the IoBuffer
 */
class BufferReader
{
public:
    explicit BufferReader(std::span<const char> data) : data_(data) {}

    [[nodiscard]] size_t Available() const { return data_.size() - offset_; }

    std::optional<std::string_view> ReadLine()
    {
        std::string_view sv(data_.data() + offset_, Available());
        auto pos = sv.find("\r\n");
        if (pos == std::string_view::npos)
            return std::nullopt;
        std::string_view line = sv.substr(0, pos);
        offset_ += (pos + 2);
        return line;
    }

    std::optional<std::span<const char>> ReadBytes(size_t n)
    {
        if (Available() < n)
            return std::nullopt;
        auto res = data_.subspan(offset_, n);
        offset_ += n;
        return res;
    }

    [[nodiscard]] size_t Offset() const { return offset_; }

private:
    std::span<const char> data_;
    size_t offset_ = 0;
};

// --- Helpers ---
std::string_view GetSimplePayload(const FrameHeader& frame);
std::string_view GetBulkPayload(const FrameHeader& frame);

}  // namespace kio::resp