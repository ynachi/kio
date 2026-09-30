#pragma once

#include <expected>
#include <utility>

namespace URing::detail {

template <typename T, typename E>
[[nodiscard]] std::unexpected<E> as_unexpected(std::expected<T, E>&& value) {
    return std::unexpected<E>(std::move(value).error());
}

}  // namespace URing::detail

// Portable Rust-`?`-style propagation. Unlike the old GNU statement-expression
// macro, these are standard C++ and are safe inside unbraced control flow.
#define KIO_DETAIL_CONCAT_INNER(LEFT, RIGHT) LEFT##RIGHT
#define KIO_DETAIL_CONCAT(LEFT, RIGHT) KIO_DETAIL_CONCAT_INNER(LEFT, RIGHT)

#define KIO_DETAIL_TRY(EXPR, RESULT)                                \
    do {                                                             \
        auto RESULT = (EXPR);                                        \
        if (!RESULT) {                                               \
            return ::URing::detail::as_unexpected(std::move(RESULT)); \
        }                                                            \
    } while (false)

#define KIO_TRY(EXPR) \
    KIO_DETAIL_TRY((EXPR), KIO_DETAIL_CONCAT(kio_try_result_, __LINE__))

#define KIO_DETAIL_TRY_ASSIGN(DESTINATION, EXPR, RESULT)            \
    do {                                                             \
        auto RESULT = (EXPR);                                        \
        if (!RESULT) {                                               \
            return ::URing::detail::as_unexpected(std::move(RESULT)); \
        }                                                            \
        (DESTINATION) = std::move(RESULT).value();                   \
    } while (false)

#define KIO_TRY_ASSIGN(DESTINATION, EXPR)      \
    KIO_DETAIL_TRY_ASSIGN(DESTINATION, (EXPR), \
                           KIO_DETAIL_CONCAT(kio_try_result_, __LINE__))

// Add this to your macro definitions
#define KIO_TRY_LET(DECLARATION, EXPR)                                               \
    do {                                                                              \
        auto KIO_DETAIL_CONCAT(kio_let_res_, __LINE__) = (EXPR);                    \
        if (!KIO_DETAIL_CONCAT(kio_let_res_, __LINE__)) {                           \
            return ::URing::detail::as_unexpected(                                     \
                std::move(KIO_DETAIL_CONCAT(kio_let_res_, __LINE__)));              \
        }                                                                             \
        DECLARATION = std::move(KIO_DETAIL_CONCAT(kio_let_res_, __LINE__)).value(); \
    } while (false)
