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
} // namespace kio_try_internal

#define KIO_DETAIL_CAT_I(a, b) a##b
#define KIO_DETAIL_CAT(a, b) KIO_DETAIL_CAT_I(a, b)

// --- COROUTINE TRY ---
// Outer macro: Evaluates __COUNTER__ exactly once and passes it as 'id'
#define KIO_CO_TRY(var, ...) \
    KIO_CO_TRY_IMPL(var, __COUNTER__, __VA_ARGS__)

// Inner macro: Uses the frozen 'id' for all variable generations
#define KIO_CO_TRY_IMPL(var, id, ...)                                    \
    auto&& KIO_DETAIL_CAT(_kio_try_, id) = (__VA_ARGS__);                \
    if (!KIO_DETAIL_CAT(_kio_try_, id).has_value()) [[unlikely]]         \
        co_return std::unexpected(std::move(KIO_DETAIL_CAT(_kio_try_, id)).error()); \
    auto var = ::kio_try_internal::kio_try_unwrap_impl(std::move(KIO_DETAIL_CAT(_kio_try_, id)))


// --- REGULAR TRY ---
#define KIO_TRY(var, ...) \
    KIO_TRY_IMPL(var, __COUNTER__, __VA_ARGS__)

#define KIO_TRY_IMPL(var, id, ...)                                       \
    auto&& KIO_DETAIL_CAT(_kio_try_, id) = (__VA_ARGS__);                \
    if (!KIO_DETAIL_CAT(_kio_try_, id).has_value()) [[unlikely]]         \
        return std::unexpected(std::move(KIO_DETAIL_CAT(_kio_try_, id)).error()); \
    auto var = ::kio_try_internal::kio_try_unwrap_impl(std::move(KIO_DETAIL_CAT(_kio_try_, id)))
