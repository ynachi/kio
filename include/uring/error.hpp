#pragma once
#include <cerrno>
#include <expected>
#include <system_error>
#include <utility>

namespace URing
{
template <typename T = void>
using Result = std::expected<T, std::error_code>;

// Accept both errno and negative io_uring completion codes.
inline std::error_code make_error_code(int error) noexcept
{
    return {error < 0 ? -error : error, std::system_category()};
}
inline std::unexpected<std::error_code> error_from_errno(int error) noexcept
{
    return std::unexpected(make_error_code(error));
}
inline std::unexpected<std::error_code> error_from_errc(std::errc error) noexcept
{
    return std::unexpected(std::make_error_code(error));
}
enum class PoolError
{
    Exhausted = 1,
    SizeTooLarge
};
inline std::error_code make_error_code(PoolError error) noexcept
{
    return make_error_code(error == PoolError::Exhausted ? ENOBUFS : EMSGSIZE);
}
}  // namespace URing

#define URING_DETAIL_CAT_(a, b) a##b
#define URING_DETAIL_CAT(a, b)  URING_DETAIL_CAT_(a, b)
#define URING_TRY(decl, expr)   URING_TRY_(URING_DETAIL_CAT(_utry_, __COUNTER__), decl, expr)
#define URING_TRY_(tmp, decl, expr)                        \
    auto&& tmp = (expr);                                   \
    if (!tmp.has_value()) [[unlikely]]                     \
        co_return std::unexpected(std::move(tmp).error()); \
    decl = std::move(*tmp)
#define URING_TRY_VOID(expr) URING_TRY_VOID_(URING_DETAIL_CAT(_utry_, __COUNTER__), expr)
#define URING_TRY_VOID_(tmp, expr)                          \
    if (auto&& tmp = (expr); !tmp.has_value()) [[unlikely]] \
    co_return std::unexpected(std::move(tmp).error())
