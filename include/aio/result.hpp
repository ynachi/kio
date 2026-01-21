#pragma once
////////////////////////////////////////////////////////////////////////////////
// Standardized Error Handling
//
// Unifies the project on std::expected<T, std::error_code>.
// Allows usage of Result<int> or Result<> (defaults to void).
////////////////////////////////////////////////////////////////////////////////

#pragma once
#include <expected>
#include <system_error>

namespace aio
{

// Single definition. Defaults to void for simple success/fail functions.
template <typename T = void>
using Result = std::expected<T, std::error_code>;

// Helper to convert errno (int) to the standardized error type
inline std::unexpected<std::error_code> error_from_errno(int err)
{
    return std::unexpected(std::error_code(err, std::system_category()));
}

inline std::error_code make_error_code(int err) {
    return std::error_code{err > 0 ? err : -err, std::system_category()};
}

}  // namespace aio
