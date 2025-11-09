//
// Created by Yao ACHI on 05/10/2025.
//

#ifndef KIO_ERRORS_H
#define KIO_ERRORS_H
#include <string>
#include <string_view>
#include <sys/errno.h>  // NOLINT
#include <type_traits>
#include <utility>

namespace kio
{
    // io_uring specific error codes
    constexpr int IOURING_SQ_FULL = 1000;
    constexpr int IOURING_CQ_FULL = 1001;
    constexpr int IOURING_SQE_TOO_EARLY = 1002;
    constexpr int IOURING_CANCELLED = 1003;

    // io serialization/deserialization errors
    constexpr int IO_DESERIALIZATION = 2004;
    constexpr int IO_DATA_CORRUPTED = 2005;

    enum class IoError : int32_t
    {
        Success = 0,

        // Network errors
        WouldBlock,
        SocketNotListening,
        InvalidFileDescriptor,
        ConnectionAborted,
        ConnectionReset,
        ConnectionRefused,
        BrokenPipe,
        InvalidAddress,
        InvalidArgument,
        TooManyOpenFDs,
        TooManyOpenFilesInSystem,
        NotEnoughMemory,
        InvalidProtocol,
        ProtocolFailure,
        OperationRefused,
        AddressInUse,
        AddressNotAvailable,
        NetworkDown,
        NetworkUnreachable,
        NetworkReset,
        ConnectionInProgress,
        AlreadyConnected,
        NotConnected,
        NotASocket,
        DestinationAddressRequired,
        MessageTooLong,
        ProtocolNotSupported,
        SocketTypeNotSupported,
        OperationNotSupported,
        ProtocolFamilyNotSupported,
        AddressFamilyNotSupported,
        HostDown,
        HostUnreachable,

        // File operation errors
        FileNotFound,
        PermissionDenied,
        FileExists,
        IsDirectory,
        NotDirectory,
        TooManySymbolicLinks,
        FileNameTooLong,
        NoSpaceLeft,
        ReadOnlyFileSystem,
        DeviceBusy,
        TimedOut,
        Interrupted,
        IOError,
        NoDevice,
        NoSuchDeviceOrAddress,
        InvalidSeek,
        DiskQuotaExceeded,
        ValueTooLarge,
        FileTooLarge,
        DirectoryNotEmpty,
        CrossDeviceLink,
        NotBlockDevice,
        IsNamedTypeFile,

        // System resource errors
        ResourceDeadlockWouldOccur,
        TooManyLinks,
        InvalidExecutableFormat,
        IllegalByteSequence,
        FunctionNotImplemented,
        OperationCancelled,
        OwnerDied,
        StateNotRecoverable,

        // Device/Media errors
        NoSuchDevice,
        WrongMediumType,
        NoMediumFound,
        RemoteIOError,

        // Miscellaneous errors
        ArgumentListTooLong,
        BadMessage,
        IdentifierRemoved,
        NoDataAvailable,
        NoMessageAvailable,
        OutOfStreamsResources,
        NotStreamDevice,
        ResultTooLarge,
        TextFileBusy,
        TooManyUsers,

        // io_uring specific
        IOUringSQFull,
        IOUringCQFull,
        IOUringSQETooEarly,
        IOUringCancelled,

        // io serialization/deserialization errors
        IODeserialization,
        IODataCorrupted,

        // Custom application errors
        EmptyBuffer,
        Unknown
    };

    constexpr bool FatalIoError(const IoError e)
    {
        switch (e)
        {
            case IoError::InvalidFileDescriptor:
            case IoError::NotEnoughMemory:
            case IoError::ConnectionAborted:
            case IoError::SocketNotListening:
            case IoError::NetworkDown:
            case IoError::IOError:
                return true;
            default:
                return false;
        }
    }

    constexpr IoError IOErrorFromErno(const int err)
    {
        switch (err)
        {
            case 0:
                return IoError::Success;

            // Network errors
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                return IoError::WouldBlock;
            case EBADF:
                return IoError::InvalidFileDescriptor;
            case ECONNABORTED:
                return IoError::ConnectionAborted;
            case ECONNRESET:
                return IoError::ConnectionReset;
            case ECONNREFUSED:
                return IoError::ConnectionRefused;
            case EPIPE:
                return IoError::BrokenPipe;
            case EFAULT:
                return IoError::InvalidAddress;
            case EINVAL:
                return IoError::InvalidArgument;
            case EMFILE:
                return IoError::TooManyOpenFDs;
            case ENFILE:
                return IoError::TooManyOpenFilesInSystem;
            case ENOMEM:
            case ENOBUFS:
                return IoError::NotEnoughMemory;
            case EOPNOTSUPP:
                return IoError::OperationNotSupported;
            case EPROTO:
                return IoError::ProtocolFailure;
            case EPERM:
                return IoError::OperationRefused;
            case EADDRINUSE:
                return IoError::AddressInUse;
            case EADDRNOTAVAIL:
                return IoError::AddressNotAvailable;
            case ENETDOWN:
                return IoError::NetworkDown;
            case ENETUNREACH:
                return IoError::NetworkUnreachable;
            case ENETRESET:
                return IoError::NetworkReset;
            case EINPROGRESS:
                return IoError::ConnectionInProgress;
            case EISCONN:
                return IoError::AlreadyConnected;
            case ENOTCONN:
                return IoError::NotConnected;
            case ENOTSOCK:
                return IoError::NotASocket;
            case EDESTADDRREQ:
                return IoError::DestinationAddressRequired;
            case EMSGSIZE:
                return IoError::MessageTooLong;
            case EPROTONOSUPPORT:
                return IoError::ProtocolNotSupported;
            case ESOCKTNOSUPPORT:
                return IoError::SocketTypeNotSupported;
            case EAFNOSUPPORT:
                return IoError::AddressFamilyNotSupported;
            case EPFNOSUPPORT:
                return IoError::ProtocolFamilyNotSupported;
            case EHOSTDOWN:
                return IoError::HostDown;
            case EHOSTUNREACH:
                return IoError::HostUnreachable;

            // File operation errors
            case ENOENT:
                return IoError::FileNotFound;
            case EACCES:
                return IoError::PermissionDenied;
            case EEXIST:
                return IoError::FileExists;
            case EISDIR:
                return IoError::IsDirectory;
            case ENOTDIR:
                return IoError::NotDirectory;
            case ELOOP:
                return IoError::TooManySymbolicLinks;
            case ENAMETOOLONG:
                return IoError::FileNameTooLong;
            case ENOSPC:
                return IoError::NoSpaceLeft;
            case EROFS:
                return IoError::ReadOnlyFileSystem;
            case EBUSY:
                return IoError::DeviceBusy;
#ifdef ETIME
            case ETIME:
#endif
            case ETIMEDOUT:
                return IoError::TimedOut;
            case EINTR:
                return IoError::Interrupted;
            case EIO:
                return IoError::IOError;
            case ENODEV:
                return IoError::NoDevice;
            case ENXIO:
                return IoError::NoSuchDeviceOrAddress;
            case ESPIPE:
                return IoError::InvalidSeek;
            case EDQUOT:
                return IoError::DiskQuotaExceeded;
            case EOVERFLOW:
                return IoError::ValueTooLarge;
            case EFBIG:
                return IoError::FileTooLarge;
            case ENOTEMPTY:
                return IoError::DirectoryNotEmpty;
            case EXDEV:
                return IoError::CrossDeviceLink;
            case ENOTBLK:
                return IoError::NotBlockDevice;

            // System resource errors
            case EDEADLK:
                return IoError::ResourceDeadlockWouldOccur;
            case EMLINK:
                return IoError::TooManyLinks;
            case ENOEXEC:
                return IoError::InvalidExecutableFormat;
            case EILSEQ:
                return IoError::IllegalByteSequence;
            case ENOSYS:
                return IoError::FunctionNotImplemented;
            case ECANCELED:
                return IoError::OperationCancelled;
            case EOWNERDEAD:
                return IoError::OwnerDied;
            case ENOTRECOVERABLE:
                return IoError::StateNotRecoverable;

                // Device/Media errors
#ifdef ENOMEDIUM
            case ENOMEDIUM:
                return IoError::NoMediumFound;
#endif
#ifdef EMEDIUMTYPE
            case EMEDIUMTYPE:
                return IoError::WrongMediumType;
#endif
#ifdef EREMOTEIO
            case EREMOTEIO:
                return IoError::RemoteIOError;
#endif

            // Miscellaneous
            case E2BIG:
                return IoError::ArgumentListTooLong;
            case EBADMSG:
                return IoError::BadMessage;
            case EIDRM:
                return IoError::IdentifierRemoved;
#ifdef ENODATA
            case ENODATA:
                return IoError::NoDataAvailable;
#endif
#ifdef ENOSR
            case ENOSR:
                return IoError::OutOfStreamsResources;
#endif
#ifdef ENOSTR
            case ENOSTR:
                return IoError::NotStreamDevice;
#endif
#ifdef ENOMSG
            case ENOMSG:
                return IoError::NoMessageAvailable;
#endif
            case ERANGE:
                return IoError::ResultTooLarge;
            case ETXTBSY:
                return IoError::TextFileBusy;
            case EUSERS:
                return IoError::TooManyUsers;

            // io_uring specific pseudo-errors
            case IOURING_SQ_FULL:
                return IoError::IOUringSQFull;
            case IOURING_CQ_FULL:
                return IoError::IOUringCQFull;
            case IOURING_SQE_TOO_EARLY:
                return IoError::IOUringSQETooEarly;
            case IOURING_CANCELLED:
                return IoError::IOUringCancelled;

            // io serialization and des
            case IO_DESERIALIZATION:
                return IoError::IODeserialization;
            case IO_DATA_CORRUPTED:
                return IoError::IODataCorrupted;

            default:
                return IoError::Unknown;
        }
    }

    constexpr std::string_view IoErrorToString(const IoError err)
    {
        switch (err)
        {
            case IoError::Success:
                return "Success";

            // Network errors
            case IoError::WouldBlock:
                return "Operation would block on a non-blocking socket or file descriptor";
            case IoError::SocketNotListening:
                return "Socket is not in listening state";
            case IoError::InvalidFileDescriptor:
                return "Invalid file descriptor - may be closed or not opened";
            case IoError::ConnectionAborted:
                return "Connection aborted by peer";
            case IoError::ConnectionReset:
                return "Connection reset by peer";
            case IoError::ConnectionRefused:
                return "Connection refused by peer";
            case IoError::BrokenPipe:
                return "Broken pipe - write to pipe with no readers";
            case IoError::InvalidAddress:
                return "Invalid address - not in writable user address space";
            case IoError::InvalidArgument:
                return "Invalid argument provided";
            case IoError::TooManyOpenFDs:
                return "Too many open file descriptors (per-process limit)";
            case IoError::TooManyOpenFilesInSystem:
                return "Too many open files (system-wide limit)";
            case IoError::NotEnoughMemory:
                return "Not enough memory available";
            case IoError::InvalidProtocol:
                return "Invalid protocol";
            case IoError::ProtocolFailure:
                return "Protocol error";
            case IoError::OperationRefused:
                return "Operation refused (possibly by firewall)";
            case IoError::AddressInUse:
                return "Address already in use";
            case IoError::AddressNotAvailable:
                return "Cannot assign requested address";
            case IoError::NetworkDown:
                return "Network is down";
            case IoError::NetworkUnreachable:
                return "Network is unreachable";
            case IoError::NetworkReset:
                return "Network connection reset";
            case IoError::ConnectionInProgress:
                return "Connection already in progress";
            case IoError::AlreadyConnected:
                return "Socket is already connected";
            case IoError::NotConnected:
                return "Socket is not connected";
            case IoError::NotASocket:
                return "File descriptor is not a socket";
            case IoError::DestinationAddressRequired:
                return "Destination address required";
            case IoError::MessageTooLong:
                return "Message too long";
            case IoError::ProtocolNotSupported:
                return "Protocol not supported";
            case IoError::SocketTypeNotSupported:
                return "Socket type not supported";
            case IoError::OperationNotSupported:
                return "Operation not supported";
            case IoError::ProtocolFamilyNotSupported:
                return "Protocol family not supported";
            case IoError::AddressFamilyNotSupported:
                return "Address family not supported";
            case IoError::HostDown:
                return "Host is down";
            case IoError::HostUnreachable:
                return "Host is unreachable";

            // File operation errors
            case IoError::FileNotFound:
                return "File or directory does not exist";
            case IoError::PermissionDenied:
                return "Permission denied";
            case IoError::FileExists:
                return "File already exists";
            case IoError::IsDirectory:
                return "Is a directory (expected file)";
            case IoError::NotDirectory:
                return "Not a directory (expected directory)";
            case IoError::TooManySymbolicLinks:
                return "Too many levels of symbolic links";
            case IoError::FileNameTooLong:
                return "File name too long";
            case IoError::NoSpaceLeft:
                return "No space left on device";
            case IoError::ReadOnlyFileSystem:
                return "Read-only file system";
            case IoError::DeviceBusy:
                return "Device or resource busy";
            case IoError::TimedOut:
                return "Operation timed out";
            case IoError::Interrupted:
                return "Operation interrupted by signal";
            case IoError::IOError:
                return "Input/output error";
            case IoError::NoDevice:
                return "No such device";
            case IoError::NoSuchDeviceOrAddress:
                return "No such device or address";
            case IoError::InvalidSeek:
                return "Illegal seek operation";
            case IoError::DiskQuotaExceeded:
                return "Disk quota exceeded";
            case IoError::ValueTooLarge:
                return "Value too large for defined data type";
            case IoError::FileTooLarge:
                return "File too large";
            case IoError::DirectoryNotEmpty:
                return "Directory not empty";
            case IoError::CrossDeviceLink:
                return "Cross-device link";
            case IoError::NotBlockDevice:
                return "Block device required";
            case IoError::IsNamedTypeFile:
                return "Is a named type file";

            // System resource errors
            case IoError::ResourceDeadlockWouldOccur:
                return "Resource deadlock would occur";
            case IoError::TooManyLinks:
                return "Too many links";
            case IoError::InvalidExecutableFormat:
                return "Invalid executable format";
            case IoError::IllegalByteSequence:
                return "Illegal byte sequence";
            case IoError::FunctionNotImplemented:
                return "Function not implemented";
            case IoError::OperationCancelled:
                return "Operation cancelled";
            case IoError::OwnerDied:
                return "Owner died";
            case IoError::StateNotRecoverable:
                return "State not recoverable";

            // Device/Media errors
            case IoError::NoSuchDevice:
                return "No such device";
            case IoError::WrongMediumType:
                return "Wrong medium type";
            case IoError::NoMediumFound:
                return "No medium found";
            case IoError::RemoteIOError:
                return "Remote I/O error";

            // Miscellaneous
            case IoError::ArgumentListTooLong:
                return "Argument list too long";
            case IoError::BadMessage:
                return "Bad message";
            case IoError::IdentifierRemoved:
                return "Identifier removed";
            case IoError::NoDataAvailable:
                return "No data available";
            case IoError::NoMessageAvailable:
                return "No message of desired type";
            case IoError::OutOfStreamsResources:
                return "Out of streams resources";
            case IoError::NotStreamDevice:
                return "Device not a stream";
            case IoError::ResultTooLarge:
                return "Result too large";
            case IoError::TextFileBusy:
                return "Text file busy";
            case IoError::TooManyUsers:
                return "Too many users";

            // io_uring specific errors
            case IoError::IOUringSQFull:
                return "io_uring submission queue is full";
            case IoError::IOUringCQFull:
                return "io_uring completion queue is full";
            case IoError::IOUringSQETooEarly:
                return "io_uring submission queue entry submitted too early";
            case IoError::IOUringCancelled:
                return "io_uring operation was cancelled";

            // io deserialization
            case IoError::IODataCorrupted:
                return "IO Data Corrupted error, checksum mismatch";
            case IoError::IODeserialization:
                return "IO Deserialization error";

            // Custom errors
            case IoError::EmptyBuffer:
                return "Empty buffer";
            case IoError::Unknown:
                return "Unknown error";
        }
        return "Unknown error";
    }

    /**
     * @brief A rich error type that holds both a kio-specific error category
     * and the original system errno value for detailed debugging.
     */
    class Error
    {
    public:
        IoError category;
        int errno_value;

        constexpr Error() : category(IoError::Success), errno_value(0) {}
        constexpr Error(const IoError cat, const int err) : category(cat), errno_value(err) {}

        static Error from_errno(const int err) { return {IOErrorFromErno(err), err}; }

        static Error from_category(IoError cat) { return {cat, 0}; }

        [[nodiscard]] constexpr bool is_success() const { return category == IoError::Success; }

        [[nodiscard]] constexpr bool is_fatal() const { return FatalIoError(category); }

        [[nodiscard]] std::string message() const { return std::string(IoErrorToString(category)) + " (errno: " + std::to_string(errno_value) + ")"; }
        [[nodiscard]] constexpr std::string_view category_string() const { return IoErrorToString(category); }

        // Comparison operators
        constexpr bool operator==(const Error& other) const { return category == other.category && errno_value == other.errno_value; }

        constexpr bool operator!=(const Error& other) const { return !(*this == other); }
    };

    // Kio Result. An alias for a type which can return kio Error
    template<typename T>
    using Result = std::expected<T, Error>;

}  // namespace kio

namespace kio_try_internal
{
    // For non-void expected<T, E> -> unwrap and move out the value.
    template<typename Exp, typename Decayed = std::decay_t<Exp>, typename ValueT = Decayed::value_type>
    std::enable_if_t<!std::is_void_v<ValueT>, ValueT> kio_try_unwrap_impl(Exp&& exp)
    {
        return std::move(*exp);
    }

    // For expected<void, E> -> do nothing (returns void).
    template<typename Exp, typename Decayed = std::decay_t<Exp>, typename ValueT = Decayed::value_type>
    std::enable_if_t<std::is_void_v<ValueT>, void> kio_try_unwrap_impl(Exp&&)
    {
        return;
        // no-op for expected<void, E>
    }

}  // namespace kio_try_internal

// Macro helpers
#define KIO_TRY_CONCAT_IMPL(a, b) a##b
#define KIO_TRY_CONCAT(a, b) KIO_TRY_CONCAT_IMPL(a, b)
#define KIO_TRY_VAR(name) KIO_TRY_CONCAT(name, __LINE__)

// KIO_TRY macro that works like Rust's ? operator
// Usage:
//   KIO_TRY(co_await task_returning_expected_void())     // For Task<std::expected<void, Error>>
//   auto value = KIO_TRY(co_await task_returning_T());   // For Task<std::expected<T, Error>>
//   KIO_TRY(function_returning_expected_void())          // For std::expected<void, Error>
//   auto value = KIO_TRY(function_returning_expected())  // For std::expected<T, Error>
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
