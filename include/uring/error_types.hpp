#pragma once

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

namespace URing {

// A deliberately small error value. `operation` is diagnostic context and must
// have static storage duration; use a string literal.
struct Error {
  std::error_code code{};
  const char* operation = "";

  [[nodiscard]] static Error FromErrno(int value, const char* operation = "") noexcept {
    return {.code = {value, std::generic_category()},
            .operation = operation != nullptr ? operation : ""};
  }

  [[nodiscard]] static Error FromErrc(const std::errc value,
                                       const char* operation = "") noexcept {
    return {.code = std::make_error_code(value),
            .operation = operation != nullptr ? operation : ""};
  }

  [[nodiscard]] static std::unexpected<Error> Fail(const std::errc value,
                                                   const char* ctx = "") noexcept {
    return std::unexpected(FromErrc(value, ctx));
  }

  [[nodiscard]] static std::unexpected<Error> Fail(const int code,
                                                   const char* ctx = "") noexcept {
    return std::unexpected(FromErrno(code, ctx));
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(code);
  }

  [[nodiscard]] int Value() const noexcept { return code.value(); }
  [[nodiscard]] std::string Message() const { return code.message(); }
  [[nodiscard]] std::string_view Context() const noexcept {
    return operation != nullptr ? operation : "";
  }
};

}  // namespace URing

// Define the formatter first so that operator<< can delegate to std::format
// and keep a single source of truth for the display format.
template <>
struct std::formatter<URing::Error> : std::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const URing::Error& value, FormatContext& context) const {
    if (value.Context().empty()) {
      return std::format_to(context.out(), "{} ({})", value.Message(), value.Value());
    }
    return std::format_to(context.out(), "{}: {} ({})", value.Context(), value.Message(),
                          value.Value());
  }
};

namespace URing {

[[nodiscard]] inline bool operator==(const Error& left, const Error& right) noexcept {
  // Context explains an error; it is not part of the error's identity.
  return left.code == right.code;
}

// Delegates to std::formatter<Error> — single source of truth for formatting.
inline std::ostream& operator<<(std::ostream& os, const Error& e) {
  return os << std::format("{}", e);
}

}  // namespace URing
