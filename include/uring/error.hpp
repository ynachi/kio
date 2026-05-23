#pragma once

#include <cstdint>
#include <expected>
#include <system_error>
namespace URing
{
////////////////////////////////////////////////////////////////////////////////
// Standardized Error Handling
//
// Unifies the project on std::expected<T, std::error_code>.
// Allows usage of Result<int> or Result<> (defaults to void).
////////////////////////////////////////////////////////////////////////////////

template <typename T = void>
using Result = std::expected<T, std::error_code>;

inline std::unexpected<std::error_code> ErrorFromErrno(const int err) noexcept
{
    return std::unexpected(std::error_code(err, std::system_category()));
}

inline std::error_code MakeErrorCode(const int err) noexcept
{
    return std::error_code{err > 0 ? err : -err, std::system_category()};
}

/**
 * Generic high-level parse errors.
 * These are protocol-agnostic, allowing different parsers to map
 * specific failures to these general categories.
 */
enum class ParseError : std::uint8_t
{
    Success = 0,
    Incomplete,       // Not enough data to finish frame
    InvalidProtocol,  // Violation of protocol rules (bad characters, etc)
    Overflow,         // Data size exceeds limits
    InternalError,    // Logical failure in parser
};

/**
 * Custom error category for Parsing
 */
class ParseErrorCategory : public std::error_category
{
public:
    const char* name() const noexcept override { return "kio::ParseError"; }

    std::string message(int ev) const override
    {
        switch (static_cast<ParseError>(ev))
        {
            case ParseError::Success:
                return "Success";
            case ParseError::Incomplete:
                return "Incomplete data (need more)";
            case ParseError::InvalidProtocol:
                return "Protocol violation / Invalid format";
            case ParseError::Overflow:
                return "Data exceeds buffer or protocol limits";
            case ParseError::InternalError:
                return "Internal parsing logic error";
            default:
                return "Unknown parse error";
        }
    }
};

// Singleton instance of the category
inline const std::error_category& GetParseErrorCategory()
{
    static ParseErrorCategory instance;
    return instance;
}

// Overload make_error_code for ADL
inline std::error_code make_error_code(ParseError e)
{
    return {static_cast<int>(e), GetParseErrorCategory()};
}

}  // namespace URing

#define KIO_TRY(expr)                                \
    ({                                               \
        auto&& _res = (expr);                        \
        if (!_res)                                   \
            co_return std::unexpected(_res.error()); \
        std::move(*_res);                            \
    })
