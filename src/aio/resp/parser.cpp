#include "aio/resp/parser.h"

#include <charconv>
#include <cstring>
#include <limits>

namespace aio::resp
{

namespace
{
// Internal Parser Utils
std::optional<int64_t> ParseInt(std::string_view s)
{
    if (s.empty())
        return std::nullopt;
    int64_t val = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc() && ptr == s.data() + s.size())
        return val;
    return std::nullopt;
}

std::optional<double> ParseDouble(std::string_view s)
{
    if (s.empty())
        return std::nullopt;
    if (s == "inf" || s == "+inf")
        return std::numeric_limits<double>::infinity();
    if (s == "-inf")
        return -std::numeric_limits<double>::infinity();
    if (s == "nan")
        return std::numeric_limits<double>::quiet_NaN();
    double val{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc() && ptr == s.data() + s.size())
        return val;
    return std::nullopt;
}
}  // namespace

// --- Helpers ---
std::string_view GetSimplePayload(const FrameHeader& frame)
{
    // Simple frames are: TypeByte + Payload + \r\n
    if (frame.size < 3)
        return {};  // Minimal: + \r \n
    // frame.data includes the TypeByte. Return content between TypeByte and \r\n.
    return {frame.data + 1, frame.size - 3};
}

std::string_view GetBulkPayload(const FrameHeader& frame)
{
    // Bulk frames: Header + \r\n + Payload + \r\n
    // frame.data points to '$'.

    // We need to find the first CRLF to know where the header ends.
    std::string_view whole_frame(frame.data, frame.size);
    auto first_crlf = whole_frame.find("\r\n");
    if (first_crlf == std::string_view::npos)
        return {};

    size_t header_len = first_crlf + 2;
    if (frame.size < header_len + 2)
        return {};  // Should handle empty bulk string case "$0\r\n\r\n"

    // The payload is everything after the first CRLF, minus the trailing CRLF
    return {frame.data + header_len, frame.size - header_len - 2};
}

// --- Parser Implementation ---
std::expected<FrameHeader, ParseError> Parser::ParseFrameInternal(std::span<const char> data, size_t depth)
{
    if (depth >= config_.max_aggregate_depth)
        return std::unexpected(ParseError::Overflow);
    if (data.empty())
        return std::unexpected(ParseError::Incomplete);

    BufferReader reader(data);
    const auto type_byte = data[0];
    const auto type = static_cast<FrameType>(type_byte);

    switch (type)
    {
        case FrameType::kInteger:
        case FrameType::kSimpleString:
        case FrameType::kSimpleError:
        case FrameType::kBoolean:
        case FrameType::kDouble:
        case FrameType::kBigNumber:
        case FrameType::kNull:
        {
            auto line = reader.ReadLine();
            if (!line)
                return std::unexpected(ParseError::Incomplete);

            // Validation for specific types
            std::string_view payload = line->substr(1);
            if (type == FrameType::kInteger && !ParseInt(payload))
                return std::unexpected(ParseError::InvalidProtocol);
            if (type == FrameType::kBoolean && (payload != "t" && payload != "f"))
                return std::unexpected(ParseError::InvalidProtocol);
            if (type == FrameType::kDouble && !ParseDouble(payload))
                return std::unexpected(ParseError::InvalidProtocol);

            const size_t size = reader.Offset();
            if (size > config_.max_size)
                return std::unexpected(ParseError::Overflow);
            return FrameHeader{type, data.data(), size, 0};
        }

        case FrameType::kBulkString:
        case FrameType::kBulkError:
        case FrameType::kVerbatimString:
        {
            auto header_line = reader.ReadLine();
            if (!header_line)
                return std::unexpected(ParseError::Incomplete);

            auto len_opt = ParseInt(header_line->substr(1));
            if (!len_opt)
                return std::unexpected(ParseError::InvalidProtocol);

            int64_t len = *len_opt;
            if (len == -1)
            {
                const size_t size = reader.Offset();
                if (size > config_.max_size)
                    return std::unexpected(ParseError::Overflow);
                return FrameHeader{type, data.data(), size, 0};
            }
            if (len < 0 || (size_t)len > config_.max_size)
                return std::unexpected(ParseError::Overflow);

            // Read payload + CRLF
            if (!reader.ReadBytes((size_t)len + 2))
                return std::unexpected(ParseError::Incomplete);

            // Verify trailing CRLF
            const char* end = data.data() + reader.Offset();
            if (end[-2] != '\r' || end[-1] != '\n')
                return std::unexpected(ParseError::InvalidProtocol);

            const size_t size = reader.Offset();
            if (size > config_.max_size)
                return std::unexpected(ParseError::Overflow);
            return FrameHeader{type, data.data(), size, 0};
        }

        case FrameType::kArray:
        case FrameType::kMap:
        case FrameType::kSet:
        case FrameType::kPush:
        case FrameType::kAttribute:
        {
            auto header_line = reader.ReadLine();
            if (!header_line)
                return std::unexpected(ParseError::Incomplete);

            auto count_opt = ParseInt(header_line->substr(1));
            if (!count_opt)
                return std::unexpected(ParseError::InvalidProtocol);

            if (type == FrameType::kArray && *count_opt == -1)
            {
                const size_t size = reader.Offset();
                if (size > config_.max_size)
                    return std::unexpected(ParseError::Overflow);
                return FrameHeader{type, data.data(), size, 0};
            }

            if (*count_opt < 0)
                return std::unexpected(ParseError::InvalidProtocol);

            size_t count = (size_t)*count_opt;
            size_t items_to_parse = (type == FrameType::kMap || type == FrameType::kAttribute) ? count * 2 : count;

            size_t current_offset = reader.Offset();
            for (size_t i = 0; i < items_to_parse; ++i)
            {
                auto child = ParseFrameInternal(data.subspan(current_offset), depth + 1);
                if (!child)
                    return std::unexpected(child.error());
                current_offset += child->size;
            }

            if (current_offset > config_.max_size)
                return std::unexpected(ParseError::Overflow);
            return FrameHeader{type, data.data(), current_offset, count};
        }

        default:
            return std::unexpected(ParseError::InvalidProtocol);
    }
}

FrameIterator::FrameIterator(const FrameHeader& parent, Parser& parser, size_t depth)
    : parser_(parser),
      current_(parent.data),
      end_(parent.data + parent.size),
      remaining_(parent.element_count),
      depth_(depth)
{
    // Skip the header line
    std::string_view sv(parent.data, parent.size);
    auto pos = sv.find("\r\n");
    if (pos != std::string_view::npos)
    {
        current_ += (pos + 2);
    }

    if (parent.type == FrameType::kMap || parent.type == FrameType::kAttribute)
    {
        remaining_ *= 2;
    }
}

std::expected<FrameHeader, ParseError> FrameIterator::Next()
{
    if (remaining_ == 0)
        return std::unexpected(ParseError::InternalError);
    if (current_ >= end_)
        return std::unexpected(ParseError::InvalidProtocol);

    auto child = parser_.ParseFrameInternal({current_, static_cast<size_t>(end_ - current_)}, depth_);
    if (!child)
        return std::unexpected(child.error());

    current_ += child->size;
    remaining_--;
    return child;
}

}  // namespace aio::resp
