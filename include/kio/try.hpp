#pragma once

#include <type_traits>
#include <utility>

namespace kio_try_internal
{
template <typename Exp>
auto kio_try_unwrap_impl(Exp&& exp)
{
    using ValueT = std::decay_t<Exp>::value_type;

    if constexpr (std::is_void_v<ValueT>)
    {
        return;
    }
    else
    {
        return std::move(*std::forward<Exp>(exp));
    }
}
}  // namespace kio_try_internal

#define KIO_DETAIL_CAT_I(a, b) a##b
#define KIO_DETAIL_CAT(a, b)   KIO_DETAIL_CAT_I(a, b)

// --- COROUTINE TRY ---
// Evaluates __COUNTER__ exactly once and passes it as 'id'.
//
// ARGUMENT LIFETIME: pass an RVALUE. The expansion binds auto&& to the
// argument and then moves out of it -- handed an lvalue, the macro silently
// empties the caller's object (the expected keeps has_value() == true with
// a moved-from value). `KIO_CO_TRY(v, helper(1));`, not
// `KIO_CO_TRY(v, member_result);`.
#define KIO_CO_TRY(var, ...) KIO_CO_TRY_IMPL(var, __COUNTER__, __VA_ARGS__)

// Inner macro: Uses the frozen 'id' for all variable generations
#define KIO_CO_TRY_IMPL(var, id, ...)                                                \
    auto&& KIO_DETAIL_CAT(_kio_try_, id) = (__VA_ARGS__);                            \
    if (!KIO_DETAIL_CAT(_kio_try_, id).has_value()) [[unlikely]]                     \
        co_return std::unexpected(std::move(KIO_DETAIL_CAT(_kio_try_, id)).error()); \
    auto var = ::kio_try_internal::kio_try_unwrap_impl(std::move(KIO_DETAIL_CAT(_kio_try_, id)))

// --- COROUTINE TRY, VOID ---
// Statement form for Result<void> (or a value you don't need): propagates
// the error and binds nothing. KIO_CO_TRY cannot consume a void value --
// `auto var = <void expr>` is ill-formed -- so every void op (fsync, close,
// rename, ...) needs this form.
//
// STATEMENT SAFETY: these expand to a bare if-statement, so an unbraced
// caller `else` binds to the macro's INNER if:
//   if (a) KIO_CO_TRY_VOID(f()); else g();  // g() runs when f() SUCCEEDED
// Always brace the macro at the call site:
//   if (a) { KIO_CO_TRY_VOID(f()); } else { g(); }
#define KIO_CO_TRY_VOID(...) KIO_CO_TRY_VOID_IMPL(__COUNTER__, __VA_ARGS__)

#define KIO_CO_TRY_VOID_IMPL(id, ...)                                                                                  \
    if (auto&& KIO_DETAIL_CAT(_kio_try_, id) = (__VA_ARGS__); !KIO_DETAIL_CAT(_kio_try_, id).has_value()) [[unlikely]] \
    co_return std::unexpected(std::move(KIO_DETAIL_CAT(_kio_try_, id)).error())

// --- REGULAR TRY ---
// Same rvalue-argument contract as KIO_CO_TRY above.
#define KIO_TRY(var, ...) KIO_TRY_IMPL(var, __COUNTER__, __VA_ARGS__)

#define KIO_TRY_IMPL(var, id, ...)                                                \
    auto&& KIO_DETAIL_CAT(_kio_try_, id) = (__VA_ARGS__);                         \
    if (!KIO_DETAIL_CAT(_kio_try_, id).has_value()) [[unlikely]]                  \
        return std::unexpected(std::move(KIO_DETAIL_CAT(_kio_try_, id)).error()); \
    auto var = ::kio_try_internal::kio_try_unwrap_impl(std::move(KIO_DETAIL_CAT(_kio_try_, id)))

// --- REGULAR TRY, VOID ---
#define KIO_TRY_VOID(...) KIO_TRY_VOID_IMPL(__COUNTER__, __VA_ARGS__)

#define KIO_TRY_VOID_IMPL(id, ...)                                                                                     \
    if (auto&& KIO_DETAIL_CAT(_kio_try_, id) = (__VA_ARGS__); !KIO_DETAIL_CAT(_kio_try_, id).has_value()) [[unlikely]] \
    return std::unexpected(std::move(KIO_DETAIL_CAT(_kio_try_, id)).error())
