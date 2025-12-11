//
// Created by Yao ACHI on 05/10/2025.
// Refactored for categorization and std::format support.
//

#ifndef KIO_ERRORS_H
#define KIO_ERRORS_H

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

    // io_uring specific error codes (1000-1999)
    constexpr int kIouringSqFull = 1000;
    constexpr int kIouringCqFull = 1001;
    constexpr int kIouringSqeTooEarly = 1002;
    constexpr int kIouringCancelled = 1003;

    // io serialization/deserialization errors (2000-2999)
    constexpr int kIoDeserialization = 2004;
    constexpr int kIoDataCorrupted = 2005;
    constexpr int kIoEof = 2006;
    constexpr int kIoNeedMoreData = 2007;

    // Custom application errors (3000-3999)
    constexpr int kAppEmptyBuffer = 3000;
    constexpr int kAppUnknown = 3001;
    constexpr int kAppInvalidArg = 3002;

    // TLS/KTLS errors (4000-4999)
    constexpr int kTlsHandshakeFailed = 4000;
    constexpr int kTlsContextCreationFailed = 4001;
    constexpr int kTlsKtlsEnableFailed = 4002;
    constexpr int kTlsCertificateLoadFailed = 4003;
    constexpr int kTlsPrivateKeyLoadFailed = 4004;
    constexpr int kTlsVerificationFailed = 4005;
    constexpr int kTlsNotConnected = 4006;
    constexpr int kTlsAlreadyConnected = 4007;
    constexpr int kTlsUnsupportedCipher = 4008;
    constexpr int kTlsWantRead = 4009;
    constexpr int kTlsWantWrite = 4010;
    constexpr int kTlsShutdownFailed = 4011;

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
        Tls,  // TLS/KTLS errors
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
            case Tls:
                return "TLS";
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
        if (err >= 3000 && err < 4000) return Application;
        if (err >= 4000 && err < 5000) return Tls;

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
            // io_uring errors
            case kIouringSqFull:
                return "Submission queue full";
            case kIouringCqFull:
                return "Completion queue full";
            case kIouringSqeTooEarly:
                return "SQE submitted too early";
            case kIouringCancelled:
                return "Operation cancelled";

            // Serialization errors
            case kIoDeserialization:
                return "Deserialization failure";
            case kIoDataCorrupted:
                return "Data corrupted (checksum)";
            case kIoEof:
                return "End of file";
            case kIoNeedMoreData:
                return "Need more data";

            // Application errors
            case kAppEmptyBuffer:
                return "Buffer is empty";
            case kAppUnknown:
                return "Unknown application error";
            case kAppInvalidArg:
                return "Invalid argument";

            // TLS errors
            case kTlsHandshakeFailed:
                return "TLS handshake failed";
            case kTlsContextCreationFailed:
                return "Failed to create TLS context";
            case kTlsKtlsEnableFailed:
                return "KTLS offload not available";
            case kTlsCertificateLoadFailed:
                return "Failed to load certificate";
            case kTlsPrivateKeyLoadFailed:
                return "Failed to load private key";
            case kTlsVerificationFailed:
                return "Certificate verification failed";
            case kTlsNotConnected:
                return "TLS not connected (handshake required)";
            case kTlsAlreadyConnected:
                return "TLS already connected";
            case kTlsUnsupportedCipher:
                return "Cipher not supported by KTLS";
            case kTlsWantRead:
                return "TLS wants read";
            case kTlsWantWrite:
                return "TLS wants write";
            case kTlsShutdownFailed:
                return "TLS shutdown failed";

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
    // Parses format specifications
    static constexpr auto parse(std::format_parse_context const& ctx) { return ctx.begin(); }

    // Formats the Error into the context
    static auto format(const kio::Error& err, std::format_context& ctx)
    {
        // Example output: "Network: Connection reset by peer (104)"
        // Example output: "IOUring: Submission queue full (1000)"
        // Example output: "TLS: KTLS offload not available (4002)"
        return std::format_to(ctx.out(), "{}: {} ({})", kio::CategoryToString(err.category), kio::ErrorValueToString(err.value), err.value);
    }
};


// -------------------------------------------------------------------------
// KIO_TRY Macro
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

#define KIO_TRY(expr)                                                \
    ({                                                               \
        auto __kio_internal_res = (expr);                            \
        if (!__kio_internal_res)                                     \
        {                                                            \
            co_return std::unexpected(__kio_internal_res.error());   \
        }                                                            \
        ::kio_try_internal::kio_try_unwrap_impl(__kio_internal_res); \
    })

#endif  // KIO_ERRORS_H
