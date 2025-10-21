//
// Created by Yao ACHI on 05/10/2025.
//

#ifndef KIO_ERRORS_H
#define KIO_ERRORS_H
#include <string>
#include <string_view>
#include <sys/errno.h>

namespace kio
{
    // io_uring specific error codes
    constexpr int IOURING_SQ_FULL = 1000;
    constexpr int IOURING_CQ_FULL = 1001;
    constexpr int IOURING_SQE_TOO_EARLY = 1002;
    constexpr int IOURING_CANCELLED = 1003;

    enum class IoError : int32_t
    {
        Success = 0,
        WouldBlock,
        SocketNotListening,
        InvalidFileDescriptor,
        ConnectionAborted,
        ConnectionReset,
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

        // io_uring specific
        IOUringSQFull,
        IOUringCQFull,
        IOUringSQETooEarly,
        IOUringCancelled,

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
            case EAGAIN:
                return IoError::WouldBlock;
            case EBADF:
                return IoError::InvalidFileDescriptor;
            case ECONNABORTED:
                return IoError::ConnectionAborted;
            case ECONNRESET:
                return IoError::ConnectionReset;
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
                return IoError::InvalidProtocol;
            case EPROTO:
                return IoError::ProtocolFailure;
            case EPERM:
                return IoError::OperationRefused;
            case EADDRINUSE:
                return IoError::AddressInUse;
            case EADDRNOTAVAIL:
                return IoError::AddressNotAvailable;

            // File operation specific errors
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

            // io_uring specific pseudo-errors
            case IOURING_SQ_FULL:
                return IoError::IOUringSQFull;
            case IOURING_CQ_FULL:
                return IoError::IOUringCQFull;
            case IOURING_SQE_TOO_EARLY:
                return IoError::IOUringSQETooEarly;
            case IOURING_CANCELLED:
                return IoError::IOUringCancelled;

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
            case IoError::WouldBlock:
                return "Operation would block on a non-blocking socket or file descriptor";
            case IoError::SocketNotListening:
                return "Socket is not in listening state";
            case IoError::InvalidFileDescriptor:
                return "This file descriptor is not valid, maybe it was closed or not opened";
            case IoError::ConnectionAborted:
                return "Connection was aborted by the peer";
            case IoError::ConnectionReset:
                return "Connection reset by peer";
            case IoError::BrokenPipe:
                return "Broken pipe - write to a pipe with no readers";
            case IoError::InvalidAddress:
                return "The addr argument is not in a writable part of the user address space";
            case IoError::InvalidArgument:
                return "Invalid argument provided";
            case IoError::TooManyOpenFDs:
                return "Limit on the number of open file descriptors has been reached, per process or system-wide";
            case IoError::TooManyOpenFilesInSystem:
                return "System-wide limit on total open files reached";
            case IoError::NotEnoughMemory:
                return "Not enough system memory available or buffer limit reached";
            case IoError::InvalidProtocol:
                return "Operation not supported by the protocol, example accept on a non-SOCK_STREAM socket";
            case IoError::ProtocolFailure:
                return "Protocol error";
            case IoError::OperationRefused:
                return "Operation refused probably due to a firewall rule";
            case IoError::AddressInUse:
                return "Address already in use";
            case IoError::AddressNotAvailable:
                return "Cannot assign requested address";
                // File operation specific errors
            case IoError::FileNotFound:
                return "File or directory does not exist";
            case IoError::PermissionDenied:
                return "Permission denied for file operation";
            case IoError::FileExists:
                return "File already exists";
            case IoError::IsDirectory:
                return "Expected file but found directory";
            case IoError::NotDirectory:
                return "Expected directory but found file";
            case IoError::TooManySymbolicLinks:
                return "Too many levels of symbolic links";
            case IoError::FileNameTooLong:
                return "File name too long";
            case IoError::NoSpaceLeft:
                return "No space left on device";
            case IoError::ReadOnlyFileSystem:
                return "File system is read-only";
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
                return "Invalid seek operation";
            case IoError::DiskQuotaExceeded:
                return "Disk quota exceeded";
            case IoError::ValueTooLarge:
                return "Value too large for defined data type";
            case IoError::FileTooLarge:
                return "File too large";
            case IoError::DirectoryNotEmpty:
                return "Directory not empty";

            // io_uring specific errors
            case IoError::IOUringSQFull:
                return "IOUring submission queue is full";
            case IoError::IOUringCQFull:
                return "IOUring completion queue is full";
            case IoError::IOUringSQETooEarly:
                return "IOUring submission queue entry submitted too early";
            case IoError::IOUringCancelled:
                return "IOUring operation was cancelled";

            case IoError::EmptyBuffer:
                return "EmptyBuffer";
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

        static Error from_errno(const int err) { return {IOErrorFromErno(err), err}; }

        // Helper to get a full, descriptive error message.
        [[nodiscard]] std::string message() const { return std::string(IoErrorToString(category)) + " (errno: " + std::to_string(errno_value) + ")"; }
    };

    /**
     * @brief A helper macro to reduce error-handling boilerplate.
     *
     * This works like Rust's `?` operator. It evaluates an expression that
     * returns a std::expected. If the expected contains an error, the macro
     * immediately propagates the error by co_returning it from the current
     * coroutine. If it has a value, the macro unwraps and returns the value.
     */
#define KIO_TRY(expr)                                  \
    ({                                                 \
        auto&& result = (expr);                        \
        if (!result.has_value())                       \
        {                                              \
            co_return std::unexpected(result.error()); \
        }                                              \
        std::move(result.value());                     \
    })


}  // namespace kio
#endif  // KIO_ERRORS_H
