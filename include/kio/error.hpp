#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <system_error>

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
} // namespace kio
