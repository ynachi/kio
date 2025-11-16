//
// Created by Yao ACHI on 05/10/2025.
//

#ifndef KIO_ERRORS_H
#define KIO_ERRORS_H
#include <expected>
#include <format>
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
    constexpr int IO_EOF = 2006;

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
        IoEoF,

        // Custom application errors
        EmptyBuffer,
        Unknown
    };

    constexpr bool FatalIoError(const IoError e)
    {
        using enum IoError;
        switch (e)
        {
            case InvalidFileDescriptor:
            case NotEnoughMemory:
            case ConnectionAborted:
            case SocketNotListening:
            case NetworkDown:
            case IOError:
                return true;
            default:
                return false;
        }
    }

    constexpr IoError IOErrorFromErno(const int err)
    {
        using enum IoError;
        switch (err)
        {
            case 0:
                return Success;

            // Network errors
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                return WouldBlock;
            case EBADF:
                return InvalidFileDescriptor;
            case ECONNABORTED:
                return ConnectionAborted;
            case ECONNRESET:
                return ConnectionReset;
            case ECONNREFUSED:
                return ConnectionRefused;
            case EPIPE:
                return BrokenPipe;
            case EFAULT:
                return InvalidAddress;
            case EINVAL:
                return InvalidArgument;
            case EMFILE:
                return TooManyOpenFDs;
            case ENFILE:
                return TooManyOpenFilesInSystem;
            case ENOMEM:
            case ENOBUFS:
                return NotEnoughMemory;
            case EOPNOTSUPP:
                return OperationNotSupported;
            case EPROTO:
                return ProtocolFailure;
            case EPERM:
                return OperationRefused;
            case EADDRINUSE:
                return AddressInUse;
            case EADDRNOTAVAIL:
                return AddressNotAvailable;
            case ENETDOWN:
                return NetworkDown;
            case ENETUNREACH:
                return NetworkUnreachable;
            case ENETRESET:
                return NetworkReset;
            case EINPROGRESS:
                return ConnectionInProgress;
            case EISCONN:
                return AlreadyConnected;
            case ENOTCONN:
                return NotConnected;
            case ENOTSOCK:
                return NotASocket;
            case EDESTADDRREQ:
                return DestinationAddressRequired;
            case EMSGSIZE:
                return MessageTooLong;
            case EPROTONOSUPPORT:
                return ProtocolNotSupported;
            case ESOCKTNOSUPPORT:
                return SocketTypeNotSupported;
            case EAFNOSUPPORT:
                return AddressFamilyNotSupported;
            case EPFNOSUPPORT:
                return ProtocolFamilyNotSupported;
            case EHOSTDOWN:
                return HostDown;
            case EHOSTUNREACH:
                return HostUnreachable;

            // File operation errors
            case ENOENT:
                return FileNotFound;
            case EACCES:
                return PermissionDenied;
            case EEXIST:
                return FileExists;
            case EISDIR:
                return IsDirectory;
            case ENOTDIR:
                return NotDirectory;
            case ELOOP:
                return TooManySymbolicLinks;
            case ENAMETOOLONG:
                return FileNameTooLong;
            case ENOSPC:
                return NoSpaceLeft;
            case EROFS:
                return ReadOnlyFileSystem;
            case EBUSY:
                return DeviceBusy;
#ifdef ETIME
            case ETIME:
#endif
            case ETIMEDOUT:
                return TimedOut;
            case EINTR:
                return Interrupted;
            case EIO:
                return IOError;
            case ENODEV:
                return NoDevice;
            case ENXIO:
                return NoSuchDeviceOrAddress;
            case ESPIPE:
                return InvalidSeek;
            case EDQUOT:
                return DiskQuotaExceeded;
            case EOVERFLOW:
                return ValueTooLarge;
            case EFBIG:
                return FileTooLarge;
            case ENOTEMPTY:
                return DirectoryNotEmpty;
            case EXDEV:
                return CrossDeviceLink;
            case ENOTBLK:
                return NotBlockDevice;

            // System resource errors
            case EDEADLK:
                return ResourceDeadlockWouldOccur;
            case EMLINK:
                return TooManyLinks;
            case ENOEXEC:
                return InvalidExecutableFormat;
            case EILSEQ:
                return IllegalByteSequence;
            case ENOSYS:
                return FunctionNotImplemented;
            case ECANCELED:
                return OperationCancelled;
            case EOWNERDEAD:
                return OwnerDied;
            case ENOTRECOVERABLE:
                return StateNotRecoverable;

                // Device/Media errors
#ifdef ENOMEDIUM
            case ENOMEDIUM:
                return NoMediumFound;
#endif
#ifdef EMEDIUMTYPE
            case EMEDIUMTYPE:
                return WrongMediumType;
#endif
#ifdef EREMOTEIO
            case EREMOTEIO:
                return RemoteIOError;
#endif

            // Miscellaneous
            case E2BIG:
                return ArgumentListTooLong;
            case EBADMSG:
                return BadMessage;
            case EIDRM:
                return IdentifierRemoved;
#ifdef ENODATA
            case ENODATA:
                return NoDataAvailable;
#endif
#ifdef ENOSR
            case ENOSR:
                return OutOfStreamsResources;
#endif
#ifdef ENOSTR
            case ENOSTR:
                return NotStreamDevice;
#endif
#ifdef ENOMSG
            case ENOMSG:
                return NoMessageAvailable;
#endif
            case ERANGE:
                return ResultTooLarge;
            case ETXTBSY:
                return TextFileBusy;
            case EUSERS:
                return TooManyUsers;

            // io_uring specific pseudo-errors
            case IOURING_SQ_FULL:
                return IOUringSQFull;
            case IOURING_CQ_FULL:
                return IOUringCQFull;
            case IOURING_SQE_TOO_EARLY:
                return IOUringSQETooEarly;
            case IOURING_CANCELLED:
                return IOUringCancelled;

            // io serialization and des
            case IO_DESERIALIZATION:
                return IODeserialization;
            case IO_DATA_CORRUPTED:
                return IODataCorrupted;
            case IO_EOF:
                return IoEoF;

            default:
                return Unknown;
        }
    }

    constexpr std::string_view IoErrorToString(const IoError err)
    {
        using enum IoError;
        switch (err)
        {
            case Success:
                return "Success";

            // Network errors
            case WouldBlock:
                return "Operation would block on a non-blocking socket or file descriptor";
            case SocketNotListening:
                return "Socket is not in listening state";
            case InvalidFileDescriptor:
                return "Invalid file descriptor - may be closed or not opened";
            case ConnectionAborted:
                return "Connection aborted by peer";
            case ConnectionReset:
                return "Connection reset by peer";
            case ConnectionRefused:
                return "Connection refused by peer";
            case BrokenPipe:
                return "Broken pipe - write to pipe with no readers";
            case InvalidAddress:
                return "Invalid address - not in writable user address space";
            case InvalidArgument:
                return "Invalid argument provided";
            case TooManyOpenFDs:
                return "Too many open file descriptors (per-process limit)";
            case TooManyOpenFilesInSystem:
                return "Too many open files (system-wide limit)";
            case NotEnoughMemory:
                return "Not enough memory available";
            case InvalidProtocol:
                return "Invalid protocol";
            case ProtocolFailure:
                return "Protocol error";
            case OperationRefused:
                return "Operation refused (possibly by firewall)";
            case AddressInUse:
                return "Address already in use";
            case AddressNotAvailable:
                return "Cannot assign requested address";
            case NetworkDown:
                return "Network is down";
            case NetworkUnreachable:
                return "Network is unreachable";
            case NetworkReset:
                return "Network connection reset";
            case ConnectionInProgress:
                return "Connection already in progress";
            case AlreadyConnected:
                return "Socket is already connected";
            case NotConnected:
                return "Socket is not connected";
            case NotASocket:
                return "File descriptor is not a socket";
            case DestinationAddressRequired:
                return "Destination address required";
            case MessageTooLong:
                return "Message too long";
            case ProtocolNotSupported:
                return "Protocol not supported";
            case SocketTypeNotSupported:
                return "Socket type not supported";
            case OperationNotSupported:
                return "Operation not supported";
            case ProtocolFamilyNotSupported:
                return "Protocol family not supported";
            case AddressFamilyNotSupported:
                return "Address family not supported";
            case HostDown:
                return "Host is down";
            case HostUnreachable:
                return "Host is unreachable";

            // File operation errors
            case FileNotFound:
                return "File or directory does not exist";
            case PermissionDenied:
                return "Permission denied";
            case FileExists:
                return "File already exists";
            case IsDirectory:
                return "Is a directory (expected file)";
            case NotDirectory:
                return "Not a directory (expected directory)";
            case TooManySymbolicLinks:
                return "Too many levels of symbolic links";
            case FileNameTooLong:
                return "File name too long";
            case NoSpaceLeft:
                return "No space left on device";
            case ReadOnlyFileSystem:
                return "Read-only file system";
            case DeviceBusy:
                return "Device or resource busy";
            case TimedOut:
                return "Operation timed out";
            case Interrupted:
                return "Operation interrupted by signal";
            case IOError:
                return "Input/output error";
            case NoDevice:
                return "No such device";
            case NoSuchDeviceOrAddress:
                return "No such device or address";
            case InvalidSeek:
                return "Illegal seek operation";
            case DiskQuotaExceeded:
                return "Disk quota exceeded";
            case ValueTooLarge:
                return "Value too large for defined data type";
            case FileTooLarge:
                return "File too large";
            case DirectoryNotEmpty:
                return "Directory not empty";
            case CrossDeviceLink:
                return "Cross-device link";
            case NotBlockDevice:
                return "Block device required";
            case IsNamedTypeFile:
                return "Is a named type file";

            // System resource errors
            case ResourceDeadlockWouldOccur:
                return "Resource deadlock would occur";
            case TooManyLinks:
                return "Too many links";
            case InvalidExecutableFormat:
                return "Invalid executable format";
            case IllegalByteSequence:
                return "Illegal byte sequence";
            case FunctionNotImplemented:
                return "Function not implemented";
            case OperationCancelled:
                return "Operation cancelled";
            case OwnerDied:
                return "Owner died";
            case StateNotRecoverable:
                return "State not recoverable";

            // Device/Media errors
            case NoSuchDevice:
                return "No such device";
            case WrongMediumType:
                return "Wrong medium type";
            case NoMediumFound:
                return "No medium found";
            case RemoteIOError:
                return "Remote I/O error";

            // Miscellaneous
            case ArgumentListTooLong:
                return "Argument list too long";
            case BadMessage:
                return "Bad message";
            case IdentifierRemoved:
                return "Identifier removed";
            case NoDataAvailable:
                return "No data available";
            case NoMessageAvailable:
                return "No message of desired type";
            case OutOfStreamsResources:
                return "Out of streams resources";
            case NotStreamDevice:
                return "Device not a stream";
            case ResultTooLarge:
                return "Result too large";
            case TextFileBusy:
                return "Text file busy";
            case TooManyUsers:
                return "Too many users";

            // io_uring specific errors
            case IOUringSQFull:
                return "io_uring submission queue is full";
            case IOUringCQFull:
                return "io_uring completion queue is full";
            case IOUringSQETooEarly:
                return "io_uring submission queue entry submitted too early";
            case IOUringCancelled:
                return "io_uring operation was cancelled";

            // io deserialization
            case IODataCorrupted:
                return "IO Data Corrupted error, checksum mismatch";
            case IODeserialization:
                return "IO Deserialization error";
            case IoEoF:
                return "IO End of File error";

            // Custom errors
            case EmptyBuffer:
                return "Empty buffer";
            case Unknown:
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

        [[nodiscard]] std::string message() const { return std::format("{} (errno: {})", category_string(), errno_value); }
        [[nodiscard]] constexpr std::string_view category_string() const { return IoErrorToString(category); }

        // Comparison operators
        constexpr bool operator==(const Error& other) const = default;
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

/// KIO_TRY macro that works like Rust's ? operator
/// Usage:
///   KIO_TRY(co_await task_returning_expected_void())     // For Task<std::expected<void, Error>>
///   auto value = KIO_TRY(co_await task_returning_T());   // For Task<std::expected<T, Error>>
///   KIO_TRY(function_returning_expected_void())          // For std::expected<void, Error>
///   auto value = KIO_TRY(function_returning_expected())  // For std::expected<T, Error>
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
