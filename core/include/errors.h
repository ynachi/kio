//
// Created by Yao ACHI on 05/10/2025.
// Refactored for categorization and std::format support.
//

#ifndef KIO_ERRORS_H
#define KIO_ERRORS_H

#include <cerrno>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace kio
{
    // -------------------------------------------------------------------------
    // Specific Error Codes (Constants)
    // -------------------------------------------------------------------------

    // io_uring specific error codes
    constexpr int kIouringSqFull = 1000;
    constexpr int kIouringCqFull = 1001;
    constexpr int kIouringSqeTooEarly = 1002;
    constexpr int kIouringCancelled = 1003;

    // io serialization/deserialization errors
    constexpr int kIoDeserialization = 2004;
    constexpr int kIoDataCorrupted = 2005;
    constexpr int kIoEof = 2006;
    constexpr int kIoNeedMoreData = 2007;

    // Custom application errors
    constexpr int kAppEmptyBuffer = 3000;
    constexpr int kAppUnknown = 3001;

    // -------------------------------------------------------------------------
    // Top-Level Categories
    // -------------------------------------------------------------------------

    enum class ErrorCategory : int8_t
    {
        Success = 0,
        Network,
        File,
        SystemResource,
        Device,
        Uring,  // For io_uring specific issues
        Serialization,  // For parsing/encoding issues
        Application,  // High-level app logic
        Unknown
    };

    // -------------------------------------------------------------------------
    // Helper Functions (Internal)
    // -------------------------------------------------------------------------

    constexpr std::string_view CategoryToString(const ErrorCategory cat)
    {
        using enum ErrorCategory;
        switch (cat)
        {
            case Success:
                return "Success";
            case Network:
                return "Network";
            case File:
                return "File";
            case SystemResource:
                return "SystemResource";
            case Device:
                return "Device";
            case Uring:
                return "IOUring";
            case Serialization:
                return "Serialization";
            case Application:
                return "Application";
            case Unknown:
                return "Unknown";
        }
        return "Unknown";
    }

    constexpr ErrorCategory CategoryFromErrno(const int err)
    {
        using enum ErrorCategory;
        if (err == 0) return Success;

        // Custom Ranges
        if (err >= 1000 && err < 2000) return Uring;
        if (err >= 2000 && err < 3000) return Serialization;
        if (err >= 3000) return Application;

        // Standard POSIX mapping
        switch (err)  // NOSONAR on number of switch cases
        {
            // Network
            case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
            case EAGAIN:
#endif
            case ECONNABORTED:
            case ECONNRESET:
            case ECONNREFUSED:
            case EPIPE:
            case EFAULT:
            case EINVAL:
            case ENETDOWN:
            case ENETUNREACH:
            case ENETRESET:
            case EINPROGRESS:
            case EISCONN:
            case ENOTCONN:
            case ENOTSOCK:
            case EDESTADDRREQ:
            case EMSGSIZE:
            case EPROTONOSUPPORT:
            case EADDRINUSE:
            case EADDRNOTAVAIL:
            case ETIMEDOUT:
            case EHOSTUNREACH:
            case EHOSTDOWN:
                return Network;

            // File
            case ENOENT:
            case EACCES:
            case EEXIST:
            case EISDIR:
            case ENOTDIR:
            case ENAMETOOLONG:
            case EROFS:
            case EBADF:
            case EIO:
            case ENOSPC:
            case ESPIPE:
            case EFBIG:
            case ENOTEMPTY:
            case EXDEV:
                return File;

            // System Resource
            case ENOMEM:
            case EMFILE:
            case ENFILE:
            case EDEADLK:
            case ENOLCK:
            case ENOSYS:
            case ECANCELED:
            case EOWNERDEAD:
            case ENOTRECOVERABLE:
                return SystemResource;

            // Device
            case ENODEV:
            case ENXIO:
            case EBUSY:
                return Device;

            default:
                return Unknown;
        }
    }

    inline std::string ErrorValueToString(const int value)
    {
        // Handle Custom Codes
        switch (value)
        {
            case kIouringSqFull:
                return "Submission queue full";
            case kIouringCqFull:
                return "Completion queue full";
            case kIouringSqeTooEarly:
                return "SQE submitted too early";
            case kIouringCancelled:
                return "Operation cancelled";
            case kIoDeserialization:
                return "Deserialization failure";
            case kIoDataCorrupted:
                return "Data corrupted (checksum)";
            case kIoEof:
                return "End of file";
            case kIoNeedMoreData:
                return "Need more data";
            case kAppEmptyBuffer:
                return "Buffer is empty";
            case kAppUnknown:
                return "Unknown application error";
            default:
                break;
        }

        // Handle POSIX Codes - thread-safe using std::system_error
        if (value > 0 && value < 1000)
        {
            return std::generic_category().message(value);
        }

        return "Unknown code";
    }

    // -------------------------------------------------------------------------
    // The Error Class
    // -------------------------------------------------------------------------

    struct Error
    {
        ErrorCategory category;
        int value;  // The specific errno or custom code

        // Constructors
        constexpr Error() : category(ErrorCategory::Success), value(0) {}
        constexpr Error(const ErrorCategory cat, const int val) : category(cat), value(val) {}

        // Factories
        static Error from_errno(int err) { return {CategoryFromErrno(err), err}; }

        // Helper for custom categories with specific values
        static Error custom(ErrorCategory cat, int val) { return {cat, val}; }

        [[nodiscard]] constexpr bool is_success() const { return category == ErrorCategory::Success; }

        constexpr bool operator==(const Error& other) const = default;
    };

    template<typename T>
    using Result = std::expected<T, Error>;

}  // namespace kio

// -------------------------------------------------------------------------
// std::format Specialization
// -------------------------------------------------------------------------

template<>
struct std::formatter<kio::Error>
{
    // Parses format specifications (we don't use any custom ones, so just generic)
    static constexpr auto parse(std::format_parse_context const& ctx) { return ctx.begin(); }

    // Formats the Error into the context
    static auto format(const kio::Error& err, std::format_context& ctx)
    {
        // Example output: "Network: Connection reset by peer (104)"
        // Example output: "IOUring: Submission queue full (1000)"
        return std::format_to(ctx.out(), "{}: {} ({})", kio::CategoryToString(err.category), kio::ErrorValueToString(err.value), err.value);
    }
};


// -------------------------------------------------------------------------
// 6. KIO_TRY Macro
// -------------------------------------------------------------------------

namespace kio_try_internal
{
    template<typename Exp>
    auto kio_try_unwrap_impl(Exp&& exp)
    {
        using ValueT = std::decay_t<Exp>::value_type;

        if constexpr (!std::is_void_v<ValueT>)
        {
            return std::move(*std::forward<Exp>(exp));
        }
        // else: void return, no-op
    }
}  // namespace kio_try_internal

#define KIO_TRY_CONCAT_IMPL(a, b) a##b
#define KIO_TRY_CONCAT(a, b) KIO_TRY_CONCAT_IMPL(a, b)
#define KIO_TRY_VAR(name) KIO_TRY_CONCAT(name, __LINE__)

#define KIO_TRY(expr)                                                       \
    ({                                                                      \
        auto KIO_TRY_VAR(__kio_result) = (expr);                            \
        if (!KIO_TRY_VAR(__kio_result))                                     \
        {                                                                   \
            co_return std::unexpected(KIO_TRY_VAR(__kio_result).error());   \
        }                                                                   \
        ::kio_try_internal::kio_try_unwrap_impl(KIO_TRY_VAR(__kio_result)); \
    })

#endif  // KIO_ERRORS_H
