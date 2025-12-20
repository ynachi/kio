//
// Created by Yao ACHI on 14/12/2025.
//

#include "parser.h"

#include <charconv>
#include <cstring>

namespace kio::resp
{
static std::optional<size_t> find_crlf(std::span<const char> span)
{
    const char* ptr = span.data();
    const size_t len = span.size();
    if (len < 2) return std::nullopt;

    const char* current = ptr;
    size_t remaining = len;

    // Vectorized scan for \n
    while (remaining > 0)
    {
        const void* found = std::memchr(current, '\n', remaining);
        if (!found) return std::nullopt;

        const auto lf = static_cast<const char*>(found);
        const size_t idx = lf - ptr;

        if (idx > 0 && ptr[idx - 1] == '\r') return idx - 1;  // Return index of \r

        current = lf + 1;
        remaining = len - (current - ptr);
    }
    return std::nullopt;
}

static std::optional<int64_t> parse_int(std::string_view s)
{
    if (s.empty()) return std::nullopt;

    int64_t val;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc() && ptr == s.data() + s.size()) return val;
    return std::nullopt;
}

static std::optional<double> parse_double(std::string_view s)
{
    if (s.empty()) return std::nullopt;
    // Special values
    if (s == "inf" || s == "+inf") return std::numeric_limits<double>::infinity();
    if (s == "-inf") return -std::numeric_limits<double>::infinity();
    if (s == "nan") return std::numeric_limits<double>::quiet_NaN();

    double val;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc() && ptr == s.data() + s.size()) return val;
    return std::nullopt;
}

static std::optional<bool> parse_boolean(std::string_view s)
{
    if (s == "t") return true;
    if (s == "f") return false;
    return std::nullopt;
}

static bool is_valid_bignum(std::string_view s)
{
    if (s.empty()) return false;
    size_t i = 0;

    // Optional sign
    if (s[0] == '-' || s[0] == '+')
    {
        i = 1;
    }

    if (i >= s.size()) return false;  // " + " or " - " or empty after sign

    for (; i < s.size(); ++i)
    {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

std::string_view get_simple_payload(const FrameHeader& frame)
{
    const auto crlf = find_crlf({frame.data, frame.size});
    if (!crlf) return {};
    return {frame.data + 1, *crlf - 1};
}

std::string_view get_bulk_payload(const FrameHeader& frame)
{
    const auto header_end = find_crlf({frame.data, frame.size});
    if (!header_end) return {};

    const size_t header_len = *header_end + 2;
    if (frame.size < header_len + 2) return {};

    return {frame.data + header_len, frame.size - header_len - 2};
}

std::expected<FrameHeader, ParseError> Parser::extract_bulk_header(std::span<const char> data,
                                                                   const FrameType type) const
{
    const auto pos = find_crlf(data);
    if (!pos) return std::unexpected(ParseError::NeedMoreData);

    const size_t header_len = *pos + 2;
    const auto maybe_payload_len = parse_int({data.data() + 1, *pos - 1});

    if (!maybe_payload_len) return std::unexpected(ParseError::Atoi);

    const auto payload_len = *maybe_payload_len;

    // Handle Null Bulk: $-1\r\n
    if (payload_len == -1)
    {
        return FrameHeader{type, data.data(), header_len, 0};
    }

    if (payload_len < 0) return std::unexpected(ParseError::Atoi);

    if (static_cast<size_t>(payload_len) > config_.max_size)
    {
        return std::unexpected(ParseError::SizeOverflow);
    }

    const auto total = header_len + payload_len + 2;
    if (data.size() < total) return std::unexpected(ParseError::NeedMoreData);

    return FrameHeader{type, data.data(), total, 0};
}

std::expected<FrameHeader, ParseError> Parser::parse_frame_internal(std::span<const char> data, const size_t depth)
{
    // Check Recursion Limit
    if (depth > config_.max_aggregate_depth) return std::unexpected(ParseError::MaxDepthReached);

    if (data.empty()) return std::unexpected(ParseError::NeedMoreData);

    const auto pos = find_crlf(data);
    if (!pos) return std::unexpected(ParseError::NeedMoreData);

    switch (const auto type = static_cast<FrameType>(data[0]))
    {
        case FrameType::Integer:
        {
            // Validation: ensure it is a valid 64-bit integer
            if (!parse_int({data.data() + 1, *pos - 1}))
            {
                return std::unexpected(ParseError::Atoi);
            }
            return FrameHeader{type, data.data(), *pos + 2, 0};
        }
        case FrameType::Boolean:
        {
            if (const std::string_view payload{data.data() + 1, *pos - 1}; !parse_boolean(payload).has_value())
            {
                return std::unexpected(ParseError::MalformedFrame);
            };

            FrameHeader header{type, data.data(), *pos + 2, 0};
            return header;
        }
        case FrameType::Double:
        {
            if (const std::string_view payload{data.data() + 1, *pos - 1}; !parse_double(payload).has_value())
            {
                return std::unexpected(ParseError::MalformedFrame);
            }

            return FrameHeader{type, data.data(), *pos + 2, 0};
        }
        case FrameType::BigNumber:
        {
            if (const std::string_view payload{data.data() + 1, *pos - 1}; !is_valid_bignum(payload))
            {
                return std::unexpected(ParseError::MalformedFrame);
            }

            return FrameHeader{type, data.data(), *pos + 2, 0};
        }
        case FrameType::SimpleString:
        case FrameType::SimpleError:
        case FrameType::Null:
            return FrameHeader{type, data.data(), *pos + 2, 0};
        case FrameType::BulkString:
        case FrameType::BulkError:
        case FrameType::VerbatimString:
            return extract_bulk_header(data, type);

            // Aggregates
        case FrameType::Array:
        case FrameType::Map:
        case FrameType::Set:
        case FrameType::Push:
        case FrameType::Attribute:
        {
            const size_t header_len = *pos + 2;

            const auto maybe_count = parse_int({data.data() + 1, *pos - 1});
            if (!maybe_count || *maybe_count < 0)
            {
                return std::unexpected(ParseError::MalformedFrame);
            }

            const auto count = static_cast<size_t>(*maybe_count);
            const size_t loop_count = (type == FrameType::Map || type == FrameType::Attribute) ? count * 2 : count;

            size_t offset = header_len;
            for (size_t i = 0; i < loop_count; ++i)
            {
                const auto child = parse_frame_internal(data.subspan(offset), depth + 1);
                if (!child) return std::unexpected(child.error());
                offset += child->size;
            }

            return FrameHeader{type, data.data(), offset, count};
        }
        default:
            return std::unexpected(ParseError::Unknown);
    }
}

FrameIterator::FrameIterator(const FrameHeader& parent, Parser& parser) :
    parser_(parser), current_(parent.data), remaining_(parent.element_count), depth_(0)
{
    // Skip header
    if (const auto header_crlf = find_crlf({parent.data, parent.size}))
    {
        current_ += (*header_crlf + 2);
    }

    // Fix iteration count for Maps
    if (parent.type == FrameType::Map || parent.type == FrameType::Attribute)
    {
        remaining_ *= 2;
    }
}

std::expected<FrameHeader, ParseError> FrameIterator::next()
{
    if (remaining_ == 0) return std::unexpected(ParseError::EoIter);

    // We can pass SIZE_MAX because we know the parent was validated.
    auto child = parser_.parse_frame_internal({current_, SIZE_MAX}, depth_);

    if (!child) return std::unexpected(child.error());

    current_ += child->size;
    remaining_--;

    return child;
}
}  // namespace kio::resp
