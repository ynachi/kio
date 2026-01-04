//
// Created by Yao ACHI on 14/12/2025.
//

#include "parser.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
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
        return std::unexpected(ParseError::NeedMoreData);
    }

    const size_t kHeaderLen = *kPos + 2;
    const auto kMaybePayloadLen = ParseInt({data.data() + 1, *kPos - 1});

    if (!kMaybePayloadLen)
    {
        return std::unexpected(ParseError::Atoi);
    }

    const auto kPayloadLen = *kMaybePayloadLen;

    // Handle Null Bulk: $-1\r\n
    if (kPayloadLen == -1)
    {
        return FrameHeader{.type = type, .data = data.data(), .size = kHeaderLen, .element_count = 0};
    }

    if (kPayloadLen < 0)
    {
        return std::unexpected(ParseError::Atoi);
    }

    if (std::cmp_greater(kPayloadLen, config_.max_size))
    {
        return std::unexpected(ParseError::SizeOverflow);
    }

    const auto kTotal = kHeaderLen + kPayloadLen + 2;
    if (data.size() < kTotal)
    {
        return std::unexpected(ParseError::NeedMoreData);
    }

    const size_t kTailIdx = kHeaderLen + kPayloadLen;
    if (data[kTailIdx] != '\r' || data[kTailIdx + 1] != '\n')
    {
        return std::unexpected(ParseError::MalformedFrame);
    }

    return FrameHeader{.type = type, .data = data.data(), .size = kTotal, .element_count = 0};
}

std::expected<FrameHeader, ParseError> Parser::ParseFrameInternal(std::span<const char> data, const size_t depth)
{
    // Check Recursion Limit
    if (depth >= config_.max_aggregate_depth)
    {
        return std::unexpected(ParseError::MaxDepthReached);
    }

    if (data.empty())
    {
        return std::unexpected(ParseError::NeedMoreData);
    }

    const auto kPos = FindCrlf(data);
    if (!kPos)
    {
        return std::unexpected(ParseError::NeedMoreData);
    }

    switch (const auto kType = static_cast<FrameType>(data[0]))
    {
        case FrameType::Integer:
        {
            // Validation: ensure it is a valid 64-bit integer
            if (!ParseInt({data.data() + 1, *kPos - 1}))
            {
                return std::unexpected(ParseError::Atoi);
            }
            return FrameHeader{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
        }
        case FrameType::Boolean:
        {
            if (const std::string_view kPayload{data.data() + 1, *kPos - 1}; !ParseBoolean(kPayload).has_value())
            {
                return std::unexpected(ParseError::MalformedFrame);
            };

            FrameHeader header{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
            return header;
        }
        case FrameType::Double:
        {
            if (const std::string_view kPayload{data.data() + 1, *kPos - 1}; !ParseDouble(kPayload).has_value())
            {
                return std::unexpected(ParseError::MalformedFrame);
            }

            return FrameHeader{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
        }
        case FrameType::BigNumber:
        {
            if (const std::string_view kPayload{data.data() + 1, *kPos - 1}; !IsValidBignum(kPayload))
            {
                return std::unexpected(ParseError::MalformedFrame);
            }

            return FrameHeader{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
        }
        case FrameType::SimpleString:
        case FrameType::SimpleError:
        case FrameType::Null:
            return FrameHeader{.type = kType, .data = data.data(), .size = *kPos + 2, .element_count = 0};
        case FrameType::BulkString:
        case FrameType::BulkError:
        case FrameType::VerbatimString:
            return ExtractBulkHeader(data, kType);

            // Aggregates
        case FrameType::Array:
        case FrameType::Map:
        case FrameType::Set:
        case FrameType::Push:
        case FrameType::Attribute:
        {
            const size_t kHeaderLen = *kPos + 2;

            const auto kMaybeCount = ParseInt({data.data() + 1, *kPos - 1});

            // Allow -1 count for Arrays (Null Array)
            if (kType == FrameType::Array && kMaybeCount && *kMaybeCount == -1)
            {
                // Return as a FrameHeader with count 0 (empty) or specifically handle null-ness logic
                // For FrameIterator compatibility, treating it as 0 elements is safe,
                // but the user might need to check if it was truly null by inspecting the raw data or a flag.
                // Or better: Use size_t::max or similar to indicate null?
                // Standard redis practice: Null Array is just "not there".                // Let's return it as a valid
                // frame with 0 elements. The consumer can check the raw data "*-1" if they care about distinction vs
                // "*0".
                return FrameHeader{.type = kType, .data = data.data(), .size = kHeaderLen, .element_count = 0};
            }

            if (!kMaybeCount || *kMaybeCount < 0)
            {
                return std::unexpected(ParseError::MalformedFrame);
            }

            const auto kCount = static_cast<size_t>(*kMaybeCount);
            if ((kType == FrameType::Map || kType == FrameType::Attribute) &&
                kCount > (std::numeric_limits<size_t>::max() / 2))
            {
                return std::unexpected(ParseError::SizeOverflow);
            }
            const size_t kLoopCount = (kType == FrameType::Map || kType == FrameType::Attribute) ? kCount * 2 : kCount;

            size_t offset = kHeaderLen;
            for (size_t i = 0; i < kLoopCount; ++i)
            {
                const auto kChild = ParseFrameInternal(data.subspan(offset), depth + 1);
                if (!kChild)
                {
                    return std::unexpected(kChild.error());
                }
                if (offset > config_.max_size || kChild->size > config_.max_size - offset)
                {
                    return std::unexpected(ParseError::SizeOverflow);
                }
                offset += kChild->size;
            }

            return FrameHeader{.type = kType, .data = data.data(), .size = offset, .element_count = kCount};
        }
        default:
            return std::unexpected(ParseError::Unknown);
    }
}

FrameIterator::FrameIterator(const FrameHeader& parent, Parser& parser)
    : parser_(parser),
      current_(parent.data),
      end_(parent.data + parent.size),
      remaining_(parent.element_count),
      depth_(0)
{
    // Skip header
    if (const auto kHeaderCrlf = FindCrlf({parent.data, parent.size}))
    {
        current_ += (*kHeaderCrlf + 2);
    }

    // Double count for Maps
    if (parent.type == FrameType::Map || parent.type == FrameType::Attribute)
    {
        remaining_ *= 2;
    }
}

std::expected<FrameHeader, ParseError> FrameIterator::Next()
{
    if (remaining_ == 0)
    {
        return std::unexpected(ParseError::EoIter);
    }

    if (current_ >= end_)
    {
        return std::unexpected(ParseError::MalformedFrame);
    }

    const auto kRemainingBytes = static_cast<size_t>(end_ - current_);
    auto child = parser_.ParseFrameInternal({current_, kRemainingBytes}, depth_);

    if (!child)
    {
        return std::unexpected(child.error());
    }

    current_ += child->size;
    remaining_--;

    return child;
}
}  // namespace kio::resp
