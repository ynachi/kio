#pragma once
#include <expected>
#include <system_error>

#include "error_types.hpp"
#include "result.hpp"

namespace URing
{
enum class PoolError: uint8_t
{
    Exhausted = 1,
    SizeTooLarge
};
inline std::error_code make_error_code(PoolError error) noexcept
{
    return {error == PoolError::Exhausted ? ENOBUFS : EMSGSIZE, std::system_category()};
}
inline std::unexpected<Error> fail(PoolError error, const char* operation = "") noexcept
{
    return std::unexpected(Error{.code = make_error_code(error), .operation = operation != nullptr ? operation : ""});
}

// io_uring completion queue entries carry -errno on failure. Normalize the
// sign before handing the code to Error::from_errno, which — unlike the old
// kio make_error_code(int) — does not do this itself.
inline std::unexpected<Error> fail_cqe(int32_t res, const char* operation = "") noexcept
{
    return fail_errno(res < 0 ? -res : res, operation);
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
