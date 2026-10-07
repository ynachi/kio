#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <system_error>

#include "uring/logger.hpp"

namespace kio
{
    // A deliberately small error value. `operation` is diagnostic context and must
    // have static storage duration; use a string literal.
    struct Error
    {
        std::error_code code{};
        const char* operation = "";

        [[nodiscard]] static Error from_errno(int value, const char* operation = "") noexcept
        {
            return {
                .code = {value, std::generic_category()},
                .operation = operation != nullptr ? operation : ""
            };
        }

        [[nodiscard]] static Error from_errc(const std::errc value,
                                             const char* operation = "") noexcept
        {
            return {
                .code = std::make_error_code(value),
                .operation = operation != nullptr ? operation : ""
            };
        }

        [[nodiscard]] static std::unexpected<Error> fail_errc(const std::errc value,
                                                              const char* operation = "") noexcept
        {
            return std::unexpected(from_errc(value, operation));
        }

        [[nodiscard]] static std::unexpected<Error> fail_errno(const int code,
                                                               const char* operation = "") noexcept
        {
            return std::unexpected(from_errno(code, operation));
        }


        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(code);
        }

        [[nodiscard]] int value() const noexcept { return code.value(); }
        [[nodiscard]] std::string message() const { return code.message(); }

        [[nodiscard]] std::string_view context() const noexcept
        {
            return operation != nullptr ? operation : "";
        }

        [[nodiscard]] bool operator==(const std::error_code& rhs) const noexcept
        {
            return code == rhs;
        }

        [[nodiscard]] bool operator==(const Error& rhs) const noexcept
        {
            return code == rhs.code;
        }
    };

    template <typename T>
    using Result = std::expected<T, Error>;
} // namespace URing


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
        KIO_LOG_ERROR(fmt " | err={} [{}:{}]" __VA_OPT__(, ) __VA_ARGS__, tmp.error().message(), \
                   tmp.error().code.category().name(), tmp.error().value());                       \
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
        KIO_LOG_ERROR(fmt " | err={} [{}:{}]" __VA_OPT__(, ) __VA_ARGS__, tmp.error().message(), \
                   tmp.error().code.category().name(), tmp.error().value());                       \
        co_return std::unexpected(std::move(tmp).error());                                    \
    }

#define URING_DETAIL_LOG(LVL, ...) URING_DETAIL_CAT(KIO_LOG_, LVL)(__VA_ARGS__)
