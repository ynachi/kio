//
// Created by Yao ACHI on 14/12/2025.
//

#include "parser.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <utility>

namespace kio::resp
{

namespace
{
std::optional<size_t> FindCrlf(std::span<const char> span)
{
    const char* ptr = span.data();
    const size_t kLen = span.size();
    if (kLen < 2)
    {
        return std::nullopt;
    }

    const char* current = ptr;
    size_t remaining = kLen;

    // Vectorized scan for \n
    while (remaining > 0)
    {
        const void* found = std::memchr(current, '\n', remaining);
        if (found == nullptr)
        {
            return std::nullopt;
        }

        const auto* const kLf = static_cast<const char*>(found);

        if (const size_t kIdx = kLf - ptr; kIdx > 0 && ptr[kIdx - 1] == '\r')
        {
            return kIdx - 1;  // Return index of \r
        }

        current = kLf + 1;
        remaining = kLen - (current - ptr);
    }
    return std::nullopt;
}

std::optional<int64_t> ParseInt(std::string_view s)
{
    if (s.empty())
    {
        return std::nullopt;
    }

    int64_t val = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc() && ptr == s.data() + s.size())  // NOLINT
    {
        return val;
    }
    return std::nullopt;
}

std::optional<double> ParseDouble(std::string_view s)
{
    if (s.empty())
    {
        return std::nullopt;
    }
    // Special values
    if (s == "inf" || s == "+inf")
    {
        return std::numeric_limits<double>::infinity();
    }
    if (s == "-inf")
    {
        return -std::numeric_limits<double>::infinity();
    }
    if (s == "nan")
    {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double val{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc() && ptr == s.data() + s.size())  // NOLINT
    {
        return val;
    }
    return std::nullopt;
}

std::optional<bool> ParseBoolean(std::string_view s)
{
    if (s == "t")
    {
        return true;
    }
    if (s == "f")
    {
        return false;
    }
    return std::nullopt;
}

bool IsValidBignum(std::string_view s)
{
    if (s.empty())
    {
        return false;
    }
    size_t i = 0;

    // Optional sign
    if (s[0] == '-' || s[0] == '+')
    {
        i = 1;
    }

    if (i >= s.size())
    {
        return false;  // " + " or " - " or empty after sign
    }

    for (; i < s.size(); ++i)
    {
        if (s[i] < '0' || s[i] > '9')
        {
            return false;
        }
    }
    return true;
}
}  // namespace
std::string_view GetSimplePayload(const FrameHeader& frame)
{
    const auto kCrlf = FindCrlf({frame.data, frame.size});
    if (!kCrlf)
    {
        return {};
    }
    return {frame.data + 1, *kCrlf - 1};
}

std::string_view GetBulkPayload(const FrameHeader& frame)
{
    const auto kHeaderEnd = FindCrlf({frame.data, frame.size});
    if (!kHeaderEnd)
    {
        return {};
    }

    const size_t kHeaderLen = *kHeaderEnd + 2;
    if (frame.size < kHeaderLen + 2)
    {
        return {};
    }

    return {frame.data + kHeaderLen, frame.size - kHeaderLen - 2};
}

std::expected<FrameHeader, ParseError> Parser::ExtractBulkHeader(std::span<const char> data, const FrameType type) const
{
    const auto kPos = FindCrlf(data);
    if (!kPos)
    {
        return std::unexpected(ParseError::kNeedMoreData);
    }

    const size_t kHeaderLen = *kPos + 2;
    const auto kMaybePayloadLen = ParseInt({data.data() + 1, *kPos - 1});

    if (!kMaybePayloadLen)
    {
        return std::unexpected(ParseError::kAtoi);
    }

    const auto kPayloadLen = *kMaybePayloadLen;

    // Handle Null Bulk: $-1\r\n
    if (kPayloadLen == -1)
    {
        return FrameHeader{.type = type, .data = data.data(), .size = kHeaderLen, .element_count = 0};
    }

    if (kPayloadLen < 0)
    {
        return std::unexpected(ParseError::kAtoi);
    }

    if (std::cmp_greater(kPayloadLen, config_.max_size))
    {
        return std::unexpected(ParseError::kSizeOverflow);
    }

    const auto kTotal = kHeaderLen + static_cast<size_t>(kPayloadLen) + 2;
    if (data.size() < kTotal)
    {
        return std::unexpected(ParseError::kNeedMoreData);
    }

    // STRICT VALIDATION (Reviewer Finding 3):
    // Ensure the payload is actually followed by \r\n
    // data[kTotal - 2] must be \r, data[kTotal - 1] must be \n
    if (data[kTotal - 2] != '\r' || data[kTotal - 1] != '\n')
    {
        return std::unexpected(ParseError::kMalformedFrame);
    }

    return FrameHeader{.type = type, .data = data.data(), .size = kTotal, .element_count = 0};
}

std::expected<FrameHeader, ParseError> Parser::ParseFrameInternal(std::span<const char> data, const size_t depth)
{
    // Reviewer Finding 4: Changed > to >= for inclusive limit
    if (depth >= config_.max_aggregate_depth)
    {
        return std::unexpected(ParseError::kMaxDepthReached);
    }

    if (data.empty())
    {
        return std::unexpected(ParseError::kNeedMoreData);
    }

    const auto kPos = FindCrlf(data);
    if (!kPos)
    {
        return std::unexpected(ParseError::kNeedMoreData);
    }

    switch (const auto kType = static_cast<FrameType>(data[0]))
    {
        case FrameType::kInteger:
        {
            // Validation: ensure it is a valid 64-bit integer
            if (!ParseInt({data.data() + 1, *kPos - 1}))
            {
                return std::unexpected(ParseError::kAtoi);
            }
            return FrameHeader{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
        }
        case FrameType::kBoolean:
        {
            if (const std::string_view kPayload{data.data() + 1, *kPos - 1}; !ParseBoolean(kPayload).has_value())
            {
                return std::unexpected(ParseError::kMalformedFrame);
            };

            FrameHeader header{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
            return header;
        }
        case FrameType::kDouble:
        {
            if (const std::string_view kPayload{data.data() + 1, *kPos - 1}; !ParseDouble(kPayload).has_value())
            {
                return std::unexpected(ParseError::kMalformedFrame);
            }

            return FrameHeader{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
        }
        case FrameType::kBigNumber:
        {
            if (const std::string_view kPayload{data.data() + 1, *kPos - 1}; !IsValidBignum(kPayload))
            {
                return std::unexpected(ParseError::kMalformedFrame);
            }

            return FrameHeader{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
        }
        case FrameType::kSimpleString:
        case FrameType::kSimpleError:
        case FrameType::kNull:
            return FrameHeader{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
        case FrameType::kBulkString:
        case FrameType::kBulkError:
        case FrameType::kVerbatimString:
            return ExtractBulkHeader(data, kType);

            // Aggregates
        case FrameType::kArray:
        case FrameType::kMap:
        case FrameType::kSet:
        case FrameType::kPush:
        case FrameType::kAttribute:
        {
            const size_t kHeaderLen = *kPos + 2;

            const auto kMaybeCount = ParseInt({data.data() + 1, *kPos - 1});

            // Allow -1 count for Arrays (Null Array)
            if (kType == FrameType::kArray && kMaybeCount && *kMaybeCount == -1)
            {
                return FrameHeader{.type = kType, .data = data.data(), .size = kHeaderLen, .element_count = 0};
            }

            if (!kMaybeCount || *kMaybeCount < 0)
            {
                return std::unexpected(ParseError::kMalformedFrame);
            }

            const auto kCount = static_cast<size_t>(*kMaybeCount);

            // Reviewer Finding 2: Overflow check
            // Check if doubling for Map/Attribute would overflow
            if ((kType == FrameType::kMap || kType == FrameType::kAttribute))
            {
                if (kCount > (std::numeric_limits<size_t>::max() / 2))
                {
                    return std::unexpected(ParseError::kSizeOverflow);
                }
            }

            const size_t kLoopCount =
                (kType == FrameType::kMap || kType == FrameType::kAttribute) ? kCount * 2 : kCount;

            size_t offset = kHeaderLen;
            for (size_t i = 0; i < kLoopCount; ++i)
            {
                // Safety: Bounds check for subspan creation
                if (offset >= data.size())
                {
                    return std::unexpected(ParseError::kNeedMoreData);
                }

                const auto kChild = ParseFrameInternal(data.subspan(offset), depth + 1);
                if (!kChild)
                {
                    return std::unexpected(kChild.error());
                }
                // Reviewer Finding 2: Offset growth unchecked
                // Check for overflow of offset + child size
                if (std::numeric_limits<size_t>::max() - offset < kChild->size)
                {
                    return std::unexpected(ParseError::kSizeOverflow);
                }

                offset += kChild->size;

                // Check against config max size
                if (offset > config_.max_size)
                {
                    return std::unexpected(ParseError::kSizeOverflow);
                }
            }

            return FrameHeader{.type = kType, .data = data.data(), .size = offset, .element_count = kCount};
        }
        default:
            return std::unexpected(ParseError::kUnknown);
    }
}

FrameIterator::FrameIterator(const FrameHeader& parent, Parser& parser, size_t depth)
    : parser_(parser),
      current_(parent.data),
      end_(parent.data + parent.size),
      remaining_(parent.element_count),
      depth_(depth)
{
    // Skip header
    if (const auto kHeaderCrlf = FindCrlf({parent.data, parent.size}))
    {
        current_ += (*kHeaderCrlf + 2);
    }

    // Double count for Maps
    if (parent.type == FrameType::kMap || parent.type == FrameType::kAttribute)
    {
        remaining_ *= 2;
    }
}

std::expected<FrameHeader, ParseError> FrameIterator::Next()
{
    if (remaining_ == 0)
    {
        return std::unexpected(ParseError::kEoIter);
    }

    // Reviewer Finding 1: Bounds check using end_
    // Calculate exact remaining bytes in the parent frame
    if (current_ >= end_)
    {
        // This should technically not happen if remaining_ > 0 and frame is well-formed,
        // but it catches malformed frames safely.
        return std::unexpected(ParseError::kMalformedFrame);
    }

    const size_t kBytesAvailable = end_ - current_;

    // Pass explicit bounds instead of SIZE_MAX
    auto child = parser_.ParseFrameInternal({current_, kBytesAvailable}, depth_);

    if (!child)
    {
        return std::unexpected(child.error());
    }

    current_ += child->size;
    remaining_--;

    return child;
}
}  // namespace kio::resp