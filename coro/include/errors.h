//
// Created by Yao ACHI on 05/10/2025.
//

#ifndef KIO_ERRORS_H
#define KIO_ERRORS_H
#include <cstdint>
#include <string_view>

namespace kio
{
// io_uring specific error codes
constexpr int IOURING_SQ_FULL = 1000;
constexpr int IOURING_CQ_FULL = 1001;
constexpr int IOURING_SQE_TOO_EARLY = 1002;
constexpr int IOURING_CANCELLED = 1003;

enum class KioError : int32_t
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

constexpr bool is_fatal(const KioError e)
{
    switch (e)
    {
        case KioError::InvalidFileDescriptor:
        case KioError::NotEnoughMemory:
        case KioError::ConnectionAborted:
        case KioError::SocketNotListening:
            return true;
        default:
            return false;
    }
}

constexpr KioError kio_from_errno(const int err)
{
    switch (err)
    {
        case 0:
            return KioError::Success;
        case EAGAIN:
            return KioError::WouldBlock;
        case EBADF:
            return KioError::InvalidFileDescriptor;
        case ECONNABORTED:
            return KioError::ConnectionAborted;
        case ECONNRESET:
            return KioError::ConnectionReset;
        case EPIPE:
            return KioError::BrokenPipe;
        case EFAULT:
            return KioError::InvalidAddress;
        case EINVAL:
            return KioError::InvalidArgument;
        case EMFILE:
            return KioError::TooManyOpenFDs;
        case ENFILE:
            return KioError::TooManyOpenFilesInSystem;
        case ENOMEM:
        case ENOBUFS:
            return KioError::NotEnoughMemory;
        case EOPNOTSUPP:
            return KioError::InvalidProtocol;
        case EPROTO:
            return KioError::ProtocolFailure;
        case EPERM:
            return KioError::OperationRefused;
        case EADDRINUSE:
            return KioError::AddressInUse;
        case EADDRNOTAVAIL:
            return KioError::AddressNotAvailable;

        // File operation specific errors
        case ENOENT:
            return KioError::FileNotFound;
        case EACCES:
            return KioError::PermissionDenied;
        case EEXIST:
            return KioError::FileExists;
        case EISDIR:
            return KioError::IsDirectory;
        case ENOTDIR:
            return KioError::NotDirectory;
        case ELOOP:
            return KioError::TooManySymbolicLinks;
        case ENAMETOOLONG:
            return KioError::FileNameTooLong;
        case ENOSPC:
            return KioError::NoSpaceLeft;
        case EROFS:
            return KioError::ReadOnlyFileSystem;
        case EBUSY:
            return KioError::DeviceBusy;
        case ETIMEDOUT:
            return KioError::TimedOut;
        case EINTR:
            return KioError::Interrupted;
        case EIO:
            return KioError::IOError;
        case ENODEV:
            return KioError::NoDevice;
        case ENXIO:
            return KioError::NoSuchDeviceOrAddress;
        case ESPIPE:
            return KioError::InvalidSeek;
        case EDQUOT:
            return KioError::DiskQuotaExceeded;
        case EOVERFLOW:
            return KioError::ValueTooLarge;
        case EFBIG:
            return KioError::FileTooLarge;
        case ENOTEMPTY:
            return KioError::DirectoryNotEmpty;

        // io_uring specific pseudo-errors
        case IOURING_SQ_FULL:
            return KioError::IOUringSQFull;
        case IOURING_CQ_FULL:
            return KioError::IOUringCQFull;
        case IOURING_SQE_TOO_EARLY:
            return KioError::IOUringSQETooEarly;
        case IOURING_CANCELLED:
            return KioError::IOUringCancelled;

        default:
            return KioError::Unknown;
    }
}

constexpr std::string_view kio_error_to_string(const KioError err)
{
    switch (err)
    {
        case KioError::Success:
            return "Success";
        case KioError::WouldBlock:
            return "Operation would block on a non-blocking socket or file descriptor";
        case KioError::SocketNotListening:
            return "Socket is not in listening state";
        case KioError::InvalidFileDescriptor:
            return "This file descriptor is not valid, maybe it was closed or not opened";
        case KioError::ConnectionAborted:
            return "Connection was aborted by the peer";
        case KioError::ConnectionReset:
            return "Connection reset by peer";
        case KioError::BrokenPipe:
            return "Broken pipe - write to a pipe with no readers";
        case KioError::InvalidAddress:
            return "The addr argument is not in a writable part of the user address space";
        case KioError::InvalidArgument:
            return "Invalid argument provided";
        case KioError::TooManyOpenFDs:
            return "Limit on the number of open file descriptors has been reached, per process or system-wide";
        case KioError::TooManyOpenFilesInSystem:
            return "System-wide limit on total open files reached";
        case KioError::NotEnoughMemory:
            return "Not enough system memory available or buffer limit reached";
        case KioError::InvalidProtocol:
            return "Operation not supported by the protocol, example accept on a non-SOCK_STREAM socket";
        case KioError::ProtocolFailure:
            return "Protocol error";
        case KioError::OperationRefused:
            return "Operation refused probably due to a firewall rule";
        case KioError::AddressInUse:
            return "Address already in use";
        case KioError::AddressNotAvailable:
            return "Cannot assign requested address";
            // File operation specific errors
        case KioError::FileNotFound:
            return "File or directory does not exist";
        case KioError::PermissionDenied:
            return "Permission denied for file operation";
        case KioError::FileExists:
            return "File already exists";
        case KioError::IsDirectory:
            return "Expected file but found directory";
        case KioError::NotDirectory:
            return "Expected directory but found file";
        case KioError::TooManySymbolicLinks:
            return "Too many levels of symbolic links";
        case KioError::FileNameTooLong:
            return "File name too long";
        case KioError::NoSpaceLeft:
            return "No space left on device";
        case KioError::ReadOnlyFileSystem:
            return "File system is read-only";
        case KioError::DeviceBusy:
            return "Device or resource busy";
        case KioError::TimedOut:
            return "Operation timed out";
        case KioError::Interrupted:
            return "Operation interrupted by signal";
        case KioError::IOError:
            return "Input/output error";
        case KioError::NoDevice:
            return "No such device";
        case KioError::NoSuchDeviceOrAddress:
            return "No such device or address";
        case KioError::InvalidSeek:
            return "Invalid seek operation";
        case KioError::DiskQuotaExceeded:
            return "Disk quota exceeded";
        case KioError::ValueTooLarge:
            return "Value too large for defined data type";
        case KioError::FileTooLarge:
            return "File too large";
        case KioError::DirectoryNotEmpty:
            return "Directory not empty";

        // io_uring specific errors
        case KioError::IOUringSQFull:
            return "IOUring submission queue is full";
        case KioError::IOUringCQFull:
            return "IOUring completion queue is full";
        case KioError::IOUringSQETooEarly:
            return "IOUring submission queue entry submitted too early";
        case KioError::IOUringCancelled:
            return "IOUring operation was cancelled";

        case KioError::EmptyBuffer:
            return "EmptyBuffer";
        case KioError::Unknown:
            return "Unknown error";
    }
    return "Unknown error";
}

}
#endif  // KIO_ERRORS_H
