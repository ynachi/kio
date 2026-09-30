#pragma once

#include <expected>
#include <system_error>

#include "error_types.hpp"

namespace URing {

template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(const std::error_code code,
                                                 const char* operation = "") noexcept {
  return std::unexpected(
      Error{.code = code, .operation = operation != nullptr ? operation : ""});
}

[[nodiscard]] inline std::unexpected<Error> fail(const std::errc code,
                                                 const char* operation = "") noexcept {
  return std::unexpected(Error::FromErrc(code, operation));
}

[[nodiscard]] inline std::unexpected<Error> fail_errno(
    const int code, const char* operation = "") noexcept {
  return std::unexpected(Error::FromErrno(code, operation));
}

}  // namespace URing
