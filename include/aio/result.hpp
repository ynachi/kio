#pragma once
////////////////////////////////////////////////////////////////////////////////
// Standardized Error Handling
//
// Unifies the project on std::expected<T, std::error_code>.
// Allows usage of Result<int> or Result<> (defaults to void).
////////////////////////////////////////////////////////////////////////////////

#include <bit>
#include <cstdint>
#include <expected>
#include <system_error>

#include <openssl/err.h>

namespace aio
{
template <typename T = void>
using Result = std::expected<T, std::error_code>;

inline std::unexpected<std::error_code> ErrorFromErrno(int err)
{
    return std::unexpected(std::error_code(err, std::system_category()));
}

inline std::error_code MakeErrorCode(int err)
{
    return std::error_code{err > 0 ? err : -err, std::system_category()};
}

// ---- OpenSSL -> std::error_code ----
namespace detail
{

struct openssl_category_t final : std::error_category
{
    const char* name() const noexcept override { return "openssl"; }

    std::string message(const int ev) const override
    {
        static_assert(sizeof(int) == 4, "This helper assumes 32-bit int");
        const auto bits = std::bit_cast<uint32_t>(ev);

        char buf[256];
        ERR_error_string_n(bits, buf, sizeof(buf));
        return buf;
    }
};

inline const std::error_category& openssl_category()
{
    static openssl_category_t cat;
    return cat;
}

}  // namespace detail

inline std::unexpected<std::error_code> ErrorFromOpenSSL()
{
    // Pull at least one error
    unsigned long e = ERR_get_error();
    if (e == 0)
    {
        return std::unexpected(std::make_error_code(std::errc::protocol_error));
    }

    // Drain remaining errors; keep the last (often most informative)
    unsigned long last = e;
    while ((e = ERR_get_error()) != 0)
    {
        last = e;
    }

    const auto bits = static_cast<uint32_t>(last);
    const int ev = std::bit_cast<int>(bits);

    return std::unexpected(std::error_code(ev, detail::openssl_category()));
}

}  // namespace aio
