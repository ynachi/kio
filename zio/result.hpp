#pragma once

#include "logger.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

namespace zio
{
////////////////////////////////////////////////////////////////////////////////
// Standardized Error Handling
//
// zio APIs use Result<T>, which is std::expected<T, std::error_code>.
// Result<> defaults to void.
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
 * These are protocol-agnostic, allowing different parsers to map specific
 * failures to these general categories.
 */
enum class ParseError : std::uint8_t
{
    Success = 0,
    Incomplete,
    InvalidProtocol,
    Overflow,
    InternalError,
};

/**
 * Buffer pool errors.
 */
enum class PoolError : std::uint8_t
{
    Success = 0,
    Exhausted,
    SizeTooLarge,
    RegistrationFailed,
    AlreadyRegistered,
};

class ParseErrorCategory : public std::error_category
{
public:
    const char* name() const noexcept override { return "zio::ParseError"; }

    std::string message(int ev) const override
    {
        switch (static_cast<ParseError>(ev))
        {
            case ParseError::Success:
                return "Success";
            case ParseError::Incomplete:
                return "Incomplete data";
            case ParseError::InvalidProtocol:
                return "Protocol violation / invalid format";
            case ParseError::Overflow:
                return "Data exceeds buffer or protocol limits";
            case ParseError::InternalError:
                return "Internal parsing logic error";
            default:
                return "Unknown parse error";
        }
    }
};

class PoolErrorCategory : public std::error_category
{
public:
    const char* name() const noexcept override { return "zio::PoolError"; }

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

inline std::error_code make_error_code(ParseError e)
{
    return {static_cast<int>(e), get_parse_error_category()};
}

inline std::error_code make_error_code(PoolError e)
{
    return {static_cast<int>(e), get_pool_error_category()};
}

}  // namespace zio

//
// Rust-style Result unwrap macros for zio.
//
// These are the non-coroutine form of KIO's URING_TRY helpers. They use plain
// `return std::unexpected(...)`, so they are valid inside normal
// Result<T>-returning functions and zio fiber lambdas/functions. They do not
// use `co_return`.
//

#define ZIO_DETAIL_CAT_(a, b) a##b
#define ZIO_DETAIL_CAT(a, b)  ZIO_DETAIL_CAT_(a, b)

// Bind the unwrapped value, else return the error.
//
// @code
// zio::Result<std::size_t> write_all(zio::io_context& ctx, int fd,
//                                   std::span<const std::byte> data)
// {
//     ZIO_TRY(auto n, ctx.send(fd, data));
//     return n;
// }
// @endcode
#define ZIO_TRY(decl, expr) ZIO_TRY_(ZIO_DETAIL_CAT(_ztry_, __COUNTER__), decl, expr)
#define ZIO_TRY_(tmp, decl, expr)                        \
    auto&& tmp = (expr);                                \
    if (!tmp.has_value()) [[unlikely]]                  \
        return std::unexpected(std::move(tmp).error()); \
    decl = std::move(*tmp)

// Check a Result<void> or discard a successful value, else return the error.
//
// @code
// ZIO_TRY_VOID(write_all(ctx, fd, data));
// @endcode
#define ZIO_TRY_VOID(expr) ZIO_TRY_VOID_(ZIO_DETAIL_CAT(_ztry_, __COUNTER__), expr)
#define ZIO_TRY_VOID_(tmp, expr)                         \
    if (auto&& tmp = (expr); !tmp.has_value()) [[unlikely]] \
    return std::unexpected(std::move(tmp).error())

// Bind the unwrapped value, else log the error and return it.
//
// The format string must be a string literal so the macro can append the
// standard zio error suffix.
//
// @code
// ZIO_TRY_LOG(auto n, ctx.recv(fd, buf), "recv fd={}", fd);
// @endcode
#define ZIO_TRY_LOG(decl, expr, fmt, ...) \
    ZIO_TRY_LOG_(ZIO_DETAIL_CAT(_ztry_, __COUNTER__), decl, expr, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ZIO_TRY_LOG_(tmp, decl, expr, fmt, ...)                                           \
    auto&& tmp = (expr);                                                                 \
    if (!tmp.has_value()) [[unlikely]]                                                   \
    {                                                                                    \
        ZIO_LOG_ERROR(fmt " | err={} [{}:{}]" __VA_OPT__(, ) __VA_ARGS__,                \
                      tmp.error().message(), tmp.error().category().name(),              \
                      tmp.error().value());                                              \
        return std::unexpected(std::move(tmp).error());                                  \
    }                                                                                    \
    decl = std::move(*tmp)

// Check a Result<void> or discard a successful value, else log and return the
// error.
//
// @code
// ZIO_TRY_VOID_LOG(write_all(ctx, fd, data), "write response fd={}", fd);
// @endcode
#define ZIO_TRY_VOID_LOG(expr, fmt, ...) \
    ZIO_TRY_VOID_LOG_(ZIO_DETAIL_CAT(_ztry_, __COUNTER__), expr, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ZIO_TRY_VOID_LOG_(tmp, expr, fmt, ...)                                            \
    if (auto&& tmp = (expr); !tmp.has_value()) [[unlikely]]                               \
    {                                                                                    \
        ZIO_LOG_ERROR(fmt " | err={} [{}:{}]" __VA_OPT__(, ) __VA_ARGS__,                \
                      tmp.error().message(), tmp.error().category().name(),              \
                      tmp.error().value());                                              \
        return std::unexpected(std::move(tmp).error());                                  \
    }
