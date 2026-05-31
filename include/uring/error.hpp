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

inline std::unexpected<std::error_code> error_from_errno(const int err) noexcept
{
    return std::unexpected(std::error_code(err, std::system_category()));
}

inline std::unexpected<std::error_code> error_from_errc(std::errc err) noexcept
{
    return std::unexpected(std::make_error_code(err));
}

inline std::error_code make_error_code(const int err) noexcept
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
 * Buffer Pool errors.
 */
enum class PoolError : std::uint8_t
{
    Success = 0,
    Exhausted,
    SizeTooLarge,
    RegistrationFailed,
    AlreadyRegistered,
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

/**
 * Custom error category for Buffer Pool
 */
class PoolErrorCategory : public std::error_category
{
public:
    const char* name() const noexcept override { return "kio::PoolError"; }

    std::string message(int ev) const override
    {
        switch (static_cast<PoolError>(ev))
        {
            case PoolError::Success:
                return "Success";
            case PoolError::Exhausted:
                return "Buffer pool exhausted";
            case PoolError::SizeTooLarge:
                return "Requested size exceeds maximum buffer size";
            case PoolError::RegistrationFailed:
                return "Failed to register buffers with io_uring";
            case PoolError::AlreadyRegistered:
                return "Pool is already registered";
            default:
                return "Unknown pool error";
        }
    }
};

// Singleton instance of the categories
inline const std::error_category& get_parse_error_category()
{
    static ParseErrorCategory instance;
    return instance;
}

inline const std::error_category& get_pool_error_category()
{
    static PoolErrorCategory instance;
    return instance;
}

// Overload make_error_code for ADL
inline std::error_code make_error_code(ParseError e)
{
    return {static_cast<int>(e), get_parse_error_category()};
}

inline std::error_code make_error_code(PoolError e)
{
    return {static_cast<int>(e), get_pool_error_category()};
}

}  // namespace URing

//
// Rust style result unwrap macro
//

#define URING_DETAIL_CAT_(a, b) a##b
#define URING_DETAIL_CAT(a, b)  URING_DETAIL_CAT_(a, b)

// Bind the unwrapped value, else co_return the error.
//   URING_TRY(auto fd, co_await io.open(path, flags));   // fd is Fd
#define URING_TRY(decl, expr) URING_TRY_(URING_DETAIL_CAT(_utry_, __COUNTER__), decl, expr)
#define URING_TRY_(tmp, decl, expr)                        \
    auto&& tmp = (expr);                                   \
    if (!tmp.has_value()) [[unlikely]]                     \
        co_return std::unexpected(std::move(tmp).error()); \
    decl = std::move(*tmp)

// Check a Result<void> (or discard a value), else co_return the error. Single statement.
//   URING_TRY_VOID(co_await flush(io));
#define URING_TRY_VOID(expr) URING_TRY_VOID_(URING_DETAIL_CAT(_utry_, __COUNTER__), expr)
#define URING_TRY_VOID_(tmp, expr)                          \
    if (auto&& tmp = (expr); !tmp.has_value()) [[unlikely]] \
    co_return std::unexpected(std::move(tmp).error())

// Value form: bind result, else log at ERROR and co_return the error.
//   URING_TRY_LOG(auto fd, co_await io.open(p, flags), "open segment {}", id);
#define URING_TRY_LOG(decl, expr, fmt, ...) \
    URING_TRY_LOG_(URING_DETAIL_CAT(_utry_, __COUNTER__), decl, expr, fmt __VA_OPT__(, ) __VA_ARGS__)

#define URING_TRY_LOG_(tmp, decl, expr, fmt, ...)                                             \
    auto&& tmp = (expr);                                                                      \
    if (!tmp.has_value()) [[unlikely]]                                                        \
    {                                                                                         \
        ALOG_ERROR(fmt " | err={} [{}:{}]" __VA_OPT__(, ) __VA_ARGS__, tmp.error().message(), \
                   tmp.error().category().name(), tmp.error().value());                       \
        co_return std::unexpected(std::move(tmp).error());                                    \
    }                                                                                         \
    decl = std::move(*tmp)

// Void/discard form: check a Result<void>, else log and co_return. Single statement.
//   URING_TRY_VOID_LOG(co_await flush(io), "flush shard {}", shard_id_);
#define URING_TRY_VOID_LOG(expr, fmt, ...) \
    URING_TRY_VOID_LOG_(URING_DETAIL_CAT(_utry_, __COUNTER__), expr, fmt __VA_OPT__(, ) __VA_ARGS__)

#define URING_TRY_VOID_LOG_(tmp, expr, fmt, ...)                                              \
    if (auto&& tmp = (expr); !tmp.has_value()) [[unlikely]]                                   \
    {                                                                                         \
        ALOG_ERROR(fmt " | err={} [{}:{}]" __VA_OPT__(, ) __VA_ARGS__, tmp.error().message(), \
                   tmp.error().category().name(), tmp.error().value());                       \
        co_return std::unexpected(std::move(tmp).error());                                    \
    }

#define URING_DETAIL_LOG(LVL, ...) URING_DETAIL_CAT(ALOG_, LVL)(__VA_ARGS__)