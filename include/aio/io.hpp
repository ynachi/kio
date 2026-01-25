#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>

#include "aio/io_context.hpp"
#include "aio/ip_address.hpp"

namespace aio
{

// Forward declarations
class IoContext;
struct UringOp;

struct FDGuard
{
    int fd = -1;
    explicit FDGuard(const int f) : fd(f) {}
    ~FDGuard()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        };
    }
    FDGuard(FDGuard&& other) noexcept : fd(other.fd) { other.fd = -1; }
    FDGuard(const FDGuard&) = delete;
    [[nodiscard]] int Get() const { return fd; }
};

// -----------------------------------------------------------------------------
// Concepts & Helpers
// -----------------------------------------------------------------------------

// Matches int, or any type with a .get() -> int method (like Socket)
template <typename T>
concept FileDescriptor = std::convertible_to<T, int> || requires(const T& t) {
    { t.Get() } -> std::convertible_to<int>;
};

// Helper to extract the raw fd
constexpr int GetRawFd(const FileDescriptor auto& fd)
{
    if constexpr (std::convertible_to<decltype(fd), int>)
    {
        return static_cast<int>(fd);
    }
    else
    {
        return fd.Get();
    }
}

struct AcceptResult
{
    int fd{-1};
    net::SocketAddress addr;
};

struct AcceptOp : UringOp
{
    using UringOp::await_resume;

    int fd;
    net::SocketAddress client_addr{};

    template <FileDescriptor F>
    AcceptOp(IoContext& ctx, const F& f) : UringOp(&ctx), fd(GetRawFd(f))
    {
    }

    void PrepareSqe(io_uring_sqe* sqe)
    {
        // Point directly to the SocketAddress internal storage
        io_uring_prep_accept(sqe, fd, client_addr.GetMutable(), &client_addr.addrlen, 0);
    }

    Result<AcceptResult> await_resume()
    {
        if (res < 0)
        {
            return std::unexpected(MakeErrorCode(res));
        }
        return AcceptResult{res, client_addr};
    }
};

/// @brief Accepts an incoming connection on a listening socket.
/// @param ctx The IoContext to run on
/// @param f Listening socket file descriptor
/// @return Awaitable yielding Result<int> with the new client socket fd
///
/// @note The accepted client address is stored in the AcceptOp and can be
///       retrieved after co_await if needed.
///
/// @code
///   while (running) {
///       auto result = co_await AsyncAccept(ctx, listen_sock);
///       if (result) {
///           int client_fd = *result;
///           // Handle new connection
///       }
///   }
/// @endcode
template <FileDescriptor F>
AcceptOp AsyncAccept(IoContext& ctx, const F& f)
{
    return AcceptOp(ctx, f);
}

struct RecvOp : UringOp
{
    int fd;
    std::span<std::byte> buffer;
    int flags;

    template <FileDescriptor F>
    RecvOp(IoContext& ctx, const F& f, std::span<std::byte> buf, int flags)
        : UringOp(&ctx), fd(GetRawFd(f)), buffer(buf), flags(flags)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) { io_uring_prep_recv(sqe, fd, buffer.data(), buffer.size(), flags); }
};

/// @brief Receives data from a socket.
/// @param ctx The IoContext to run on
/// @param f Socket to receive from
/// @param buffer Buffer to receive into. MUST remain valid until operation completes.
/// @param flags Optional recv flags (default: 0)
/// @return Awaitable yielding Result<size_t> with bytes received (0 = connection closed)
///
/// @warning The buffer must remain valid until co_await returns!
/// @warning Returns 0 bytes on graceful connection close (not an error).
///
/// @code
///   std::array<std::byte, 1024> buffer;
///   auto result = co_await AsyncRecv(ctx, socket, buffer);
///   if (result && *result > 0) {
///       // Process received data
///   } else if (result && *result == 0) {
///       // Connection closed by peer
///   }
/// @endcode
template <FileDescriptor F>
RecvOp AsyncRecv(IoContext& ctx, const F& f, std::span<std::byte> buffer, int flags = 0)
{
    return RecvOp{ctx, f, buffer, flags};
}

/// @brief Receives data into a char array (convenience overload).
template <FileDescriptor F, size_t N>
RecvOp AsyncRecv(IoContext& ctx, const F& f, char (&buf)[N], int flags = 0)
{
    return RecvOp{
        ctx, f, std::span{reinterpret_cast<std::byte*>(buf), N},
          flags
    };
}

/// @brief Receives data into a std::array (convenience overload).
template <FileDescriptor F, size_t N>
RecvOp AsyncRecv(IoContext& ctx, const F& f, std::array<std::byte, N>& buf, int flags = 0)
{
    return RecvOp{ctx, f, std::span{buf}, flags};
}

struct SendOp : UringOp
{
    int fd;
    std::span<const std::byte> buffer;
    int flags;

    template <FileDescriptor F>
    SendOp(IoContext& ctx, const F& f, std::span<const std::byte> buf, int flags)
        : UringOp(&ctx), fd(GetRawFd(f)), buffer(buf), flags(flags)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_send(sqe, fd, buffer.data(), buffer.size(), flags); }
};

/// @brief Sends data to a socket.
/// @param ctx The IoContext to run on
/// @param f Socket to send to
/// @param buffer Buffer to send from. MUST remain valid until operation completes.
/// @param flags Optional send flags (default: 0)
/// @return Awaitable yielding Result<size_t> with bytes sent
///
/// @warning The buffer must remain valid until co_await returns!
/// @note May send fewer bytes than requested (partial send). Use AsyncSendExact for complete sends.
///
/// @code
///   auto data = std::as_bytes(std::span(my_data));
///   auto result = co_await AsyncSend(ctx, socket, data);
///   if (result) {
///       size_t sent = *result;  // May be < data.size()
///   }
/// @endcode
template <FileDescriptor F>
inline SendOp AsyncSend(IoContext& ctx, const F& f, std::span<const std::byte> buffer, int flags = 0)
{
    return SendOp{ctx, f, buffer, flags};
}

/// @brief Sends a string_view to a socket (convenience overload).
/// @note Perfect for sending HTTP responses or text protocols.
template <FileDescriptor F>
SendOp AsyncSend(IoContext& ctx, const F& f, std::string_view str, int flags = 0)
{
    return SendOp{
        ctx, f, std::span{reinterpret_cast<const std::byte*>(str.data()), str.size()},
          flags
    };
}

/// @brief Sends a char array to a socket (convenience overload).
/// @note Automatically excludes the null terminator.
template <FileDescriptor F, size_t N>
SendOp AsyncSend(IoContext& ctx, const F& f, const char (&buf)[N], int flags = 0)
{
    return SendOp{
        ctx, f, std::span{reinterpret_cast<const std::byte*>(buf), N - 1}, // Skip null terminator
        flags
    };
}

struct OpenOp : UringOp
{
    using UringOp::await_resume;

    const char* path;
    int flags;
    mode_t mode;

    OpenOp(IoContext& ctx, const char* p, int f, mode_t m) : UringOp(&ctx), path(p), flags(f), mode(m) {}

    void PrepareSqe(io_uring_sqe* sqe) { io_uring_prep_openat(sqe, AT_FDCWD, path, flags, mode); }

    Result<int> await_resume()
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return res;
    }
};

/// @brief Opens a file asynchronously.
/// @param ctx The IoContext to run on
/// @param path Path to the file
/// @param flags Open flags (O_RDONLY, O_CREAT, etc.)
/// @param mode File mode for creation (default 0644)
/// @return Awaitable yielding Result<int> with the new file descriptor
///
/// @code
///   auto fd_res = co_await AsyncOpen(ctx, "data.txt", O_RDONLY);
///   if (fd_res) {
///       int fd = *fd_res;
///       // Use fd...
///   }
/// @endcode
inline OpenOp AsyncOpen(IoContext& ctx, const char* path, int flags, mode_t mode = 0644)
{
    return OpenOp(ctx, path, flags, mode);
}

struct ReadOp : UringOp
{
    int fd;
    std::span<std::byte> buffer;
    uint64_t offset;

    template <FileDescriptor F>
    ReadOp(IoContext& ctx, const F& f, std::span<std::byte> buf, uint64_t off)
        : UringOp(&ctx), fd(GetRawFd(f)), buffer(buf), offset(off)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, buffer.data(), buffer.size(), offset); }
};

/// @brief Reads data from a file descriptor.
/// @param ctx The IoContext to run on
/// @param f File descriptor to read from
/// @param buffer Buffer to read into. MUST remain valid until operation completes.
/// @param offset File offset to read from (default: 0, use -1 for current position)
/// @return Awaitable yielding Result<size_t> with bytes read (0 = EOF)
///
/// @warning The buffer must remain valid until co_await returns!
///
/// @code
///   std::vector<std::byte> buffer(4096);
///   auto result = co_await AsyncRead(ctx, fd, buffer, file_offset);
///   if (result && *result > 0) {
///       // Process data
///   }
/// @endcode
template <FileDescriptor F>
ReadOp AsyncRead(IoContext& ctx, const F& f, std::span<std::byte> buffer, uint64_t offset = 0)
{
    return ReadOp{ctx, f, buffer, offset};
}

struct WriteOp : UringOp
{
    int fd;
    std::span<const std::byte> buffer;
    uint64_t offset;

    template <FileDescriptor F>
    WriteOp(IoContext& ctx, const F& f, std::span<const std::byte> buf, uint64_t off)
        : UringOp(&ctx), fd(GetRawFd(f)), buffer(buf), offset(off)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_write(sqe, fd, buffer.data(), buffer.size(), offset); }
};

/// @brief Writes data to a file descriptor.
/// @param ctx The IoContext to run on
/// @param f File descriptor to write to
/// @param buffer Buffer to write from. MUST remain valid until operation completes.
/// @param offset File offset to write at (default: 0, use -1 for current position)
/// @return Awaitable yielding Result<size_t> with bytes written
///
/// @warning The buffer must remain valid until co_await returns!
/// @note May write fewer bytes than requested. Use AsyncWriteExact for complete writes.
///
/// @code
///   auto data = std::as_bytes(std::span(record));
///   co_await AsyncWrite(ctx, fd, data, file_offset);
/// @endcode
template <FileDescriptor F>
WriteOp AsyncWrite(IoContext& ctx, const F& f, std::span<const std::byte> buffer, uint64_t offset = 0)
{
    return WriteOp{ctx, f, buffer, offset};
}

struct CloseOp : UringOp
{
    using UringOp::await_resume;

    int fd;

    template <FileDescriptor F>
    CloseOp(IoContext& ctx, const F& f) : UringOp(&ctx), fd(GetRawFd(f))
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_close(sqe, fd); }

    Result<void> await_resume()
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return {};
    }
};

struct ReadFixedOp : UringOp
{
    int file_index;
    void* buffer;
    size_t len;
    off_t offset;

    ReadFixedOp(IoContext& ctx, int idx, std::span<std::byte> buf, off_t off)
        : UringOp(&ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const
    {
        io_uring_prep_read(sqe, file_index, buffer, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    }
};

/// @brief Reads from a registered file using its index (IOSQE_FIXED_FILE).
/// @param ctx The IoContext to run on
/// @param idx Index into the registered file table (from IoContext::RegisterFiles)
/// @param buf Buffer to read into. MUST remain valid until operation completes.
/// @param off File offset to read from
/// @return Awaitable yielding Result<size_t> with bytes read
///
/// @warning The buffer must remain valid until co_await returns!
/// @note Requires files to be registered with IoContext::RegisterFiles() first.
///       Uses fixed file optimization for reduced kernel overhead.
///
/// @code
///   // Register files once at startup
///   std::array fds = {fd1, fd2, fd3};
///   ctx.RegisterFiles(fds);
///
///   // Use index instead of fd
///   co_await AsyncReadFixed(ctx, 0, buffer, offset);  // Reads from fd1
/// @endcode
inline ReadFixedOp AsyncReadFixed(IoContext& ctx, int idx, std::span<std::byte> buf, off_t off)
{
    return ReadFixedOp(ctx, idx, buf, off);
}

struct WriteFixedOp : UringOp
{
    int file_index;
    const void* buffer;
    size_t len;
    off_t offset;

    WriteFixedOp(IoContext& ctx, int idx, std::span<const std::byte> buf, off_t off)
        : UringOp(&ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe)
    {
        io_uring_prep_write(sqe, file_index, buffer, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    }
};

/// @brief Writes to a registered file using its index (IOSQE_FIXED_FILE).
/// @param ctx The IoContext to run on
/// @param idx Index into the registered file table (from IoContext::RegisterFiles)
/// @param buf Buffer to write from. MUST remain valid until operation completes.
/// @param off File offset to write at
/// @return Awaitable yielding Result<size_t> with bytes written
///
/// @warning The buffer must remain valid until co_await returns!
/// @note Requires files to be registered with IoContext::RegisterFiles() first.
///       Uses fixed file optimization for reduced kernel overhead.
inline WriteFixedOp AsyncWriteFixed(IoContext& ctx, int idx, std::span<const std::byte> buf, off_t off)
{
    return WriteFixedOp(ctx, idx, buf, off);
}

/// @brief Closes a file descriptor asynchronously.
/// @param ctx The IoContext to run on
/// @param f File descriptor to close
/// @return Awaitable yielding Result<void>
///
/// @note After co_await returns, the fd is closed and must not be used.
///
/// @code
///   co_await AsyncClose(ctx, client_socket);
///   // client_socket is now invalid
/// @endcode
template <FileDescriptor F>
CloseOp AsyncClose(IoContext& ctx, const F& f)
{
    return CloseOp(ctx, f);
}

struct ConnectOp : UringOp
{
    using UringOp::await_resume;

    int fd;
    sockaddr_storage addr_store{};
    socklen_t addrlen;

    template <FileDescriptor F>
    ConnectOp(IoContext& ctx, const F& f, const sockaddr* addr, socklen_t len)
        : UringOp(&ctx), fd(GetRawFd(f)), addrlen(len)
    {
        std::memcpy(&addr_store, addr, len);
    }

    void PrepareSqe(io_uring_sqe* sqe)
    {
        io_uring_prep_connect(sqe, fd, reinterpret_cast<sockaddr*>(&addr_store), addrlen);
    }

    Result<void> await_resume()
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return {};
    }
};

/// @brief Connects a socket to a remote address.
/// @param ctx The IoContext to run on
/// @param f Socket file descriptor (should be non-blocking)
/// @param addr Pointer to sockaddr with destination address
/// @param len Size of the sockaddr structure
/// @return Awaitable yielding Result<void>
///
/// @note The sockaddr is copied internally, so it does not need to remain valid.
///
/// @code
///   sockaddr_in server{};
///   server.sin_family = AF_INET;
///   server.sin_port = htons(8080);
///   inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);
///
///   auto result = co_await AsyncConnect(ctx, socket, (sockaddr*)&server, sizeof(server));
///   if (!result) {
///       // Connection failed
///   }
/// @endcode
template <FileDescriptor F>
ConnectOp AsyncConnect(IoContext& ctx, const F& f, const sockaddr* addr, socklen_t len)
{
    return ConnectOp(ctx, f, addr, len);
}

/// @brief Connects a socket to a remote address using SocketAddress.
/// @param ctx The IoContext to run on
/// @param f Socket file descriptor (should be non-blocking)
/// @param addr SocketAddress with destination address
/// @return Awaitable yielding Result<void>
///
/// @code
///   auto addr = aio::net::SocketAddress::V4(8080, "127.0.0.1");
///   auto result = co_await AsyncConnect(ctx, socket, addr);
/// @endcode
template <FileDescriptor F>
ConnectOp AsyncConnect(IoContext& ctx, const F& f, const net::SocketAddress& addr)
{
    return ConnectOp(ctx, f, addr.Get(), addr.addrlen);
}

struct FsyncOp : UringOp
{
    int fd;

    template <FileDescriptor F>
    FsyncOp(IoContext& ctx, const F& f) : UringOp(&ctx), fd(GetRawFd(f))
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_fsync(sqe, fd, 0); }
};

/// @brief Flushes file data and metadata to disk (fsync).
/// @param ctx The IoContext to run on
/// @param f File descriptor to sync
/// @return Awaitable yielding Result<size_t>
///
/// @note Ensures both data and metadata (size, timestamps, etc.) are persisted.
///       For data-only sync, use AsyncFdatasync which may be faster.
///
/// @code
///   co_await AsyncWrite(ctx, fd, data, offset);
///   co_await AsyncFsync(ctx, fd);  // Data is now durable
/// @endcode
template <FileDescriptor F>
FsyncOp AsyncFsync(IoContext& ctx, const F& f)
{
    return FsyncOp(ctx, f);
}

struct FdatasyncOp : UringOp
{
    int fd;

    template <FileDescriptor F>
    FdatasyncOp(IoContext& ctx, const F& f) : UringOp(&ctx), fd(GetRawFd(f))
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC); }
};

/// @brief Flushes file data to disk, skipping metadata (fdatasync).
/// @param ctx The IoContext to run on
/// @param f File descriptor to sync
/// @return Awaitable yielding Result<size_t>
///
/// @note Faster than fsync when metadata changes (like atime) don't need persistence.
///       Use this for write-ahead logs where only data durability matters.
///
/// @code
///   co_await AsyncWrite(ctx, wal_fd, log_entry, offset);
///   co_await AsyncFdatasync(ctx, wal_fd);  // Data durable, metadata may not be
/// @endcode
template <FileDescriptor F>
FdatasyncOp AsyncFdatasync(IoContext& ctx, const F& f)
{
    return FdatasyncOp(ctx, f);
}

struct FallocateOp : UringOp
{
    int fd;
    int mode;
    off_t offset;
    off_t len;

    template <FileDescriptor F>
    FallocateOp(IoContext& ctx, const F& f, int mode, off_t offset, off_t len)
        : UringOp(&ctx), fd(GetRawFd(f)), mode(mode), offset(offset), len(len)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_fallocate(sqe, fd, mode, offset, len); }
};

/// @brief Pre-allocates or manipulates file space (fallocate).
/// @param ctx The IoContext to run on
/// @param f File descriptor
/// @param mode Allocation mode (0 for default, FALLOC_FL_KEEP_SIZE, FALLOC_FL_PUNCH_HOLE, etc.)
/// @param offset Starting offset for the operation
/// @param len Number of bytes to allocate/deallocate
/// @return Awaitable yielding Result<size_t>
///
/// @note Pre-allocation avoids fragmentation and ensures space is available.
///       Common modes: 0 (allocate), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE (deallocate).
///
/// @code
///   // Pre-allocate 1GB for a database file
///   co_await AsyncFallocate(ctx, db_fd, 0, 0, 1024 * 1024 * 1024);
/// @endcode
template <FileDescriptor F>
FallocateOp AsyncFallocate(IoContext& ctx, const F& f, int mode, off_t offset, off_t len)
{
    return FallocateOp(ctx, f, mode, offset, len);
}

struct FtruncateOp : UringOp
{
    int fd;
    off_t len;

    template <FileDescriptor F>
    FtruncateOp(IoContext& ctx, const F& f, off_t len) : UringOp(&ctx), fd(GetRawFd(f)), len(len)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_ftruncate(sqe, fd, len); }
};

/// @brief Truncates or extends a file to the specified length.
/// @param ctx The IoContext to run on
/// @param f File descriptor
/// @param len New file size in bytes
/// @return Awaitable yielding Result<size_t>
///
/// @note If len < current size, data beyond len is discarded.
///       If len > current size, file is extended with zero bytes (or a hole).
///
/// @code
///   // Truncate log file after rotation
///   co_await AsyncFtruncate(ctx, log_fd, 0);
/// @endcode
template <FileDescriptor F>
FtruncateOp AsyncFtruncate(IoContext& ctx, const F& f, off_t len)
{
    return FtruncateOp(ctx, f, len);
}

struct PollOp : UringOp
{
    int fd;
    unsigned poll_mask;

    template <FileDescriptor F>
    PollOp(IoContext& ctx, const F& f, unsigned mask) : UringOp(&ctx), fd(GetRawFd(f)), poll_mask(mask)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_poll_add(sqe, fd, poll_mask); }
};

/// @brief Waits for events on a file descriptor (poll).
/// @param ctx The IoContext to run on
/// @param f File descriptor to poll
/// @param poll_mask Events to wait for (POLLIN, POLLOUT, POLLERR, etc.)
/// @return Awaitable yielding Result<size_t> with the triggered events mask
///
/// @note Useful for waiting on special fds (eventfd, timerfd, signalfd) or
///       checking socket readiness without performing I/O.
///
/// @code
///   // Wait for socket to become readable
///   auto result = co_await AsyncPoll(ctx, socket, POLLIN);
///   if (result && (*result & POLLIN)) {
///       // Socket has data ready
///   }
/// @endcode
template <FileDescriptor F>
PollOp AsyncPoll(IoContext& ctx, const F& f, unsigned poll_mask)
{
    return PollOp(ctx, f, poll_mask);
}

struct ReadvOp : UringOp
{
    int fd;
    std::span<const iovec> iovecs;
    uint64_t offset;

    template <FileDescriptor F>
    ReadvOp(IoContext& ctx, const F& f, std::span<const iovec> iov, uint64_t off)
        : UringOp(&ctx), fd(GetRawFd(f)), iovecs(iov), offset(off)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const
    {
        io_uring_prep_readv(sqe, fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset);
    }
};

/// @brief Reads data into multiple buffers (scatter read).
/// @param ctx The IoContext to run on
/// @param f File descriptor to read from
/// @param iovecs Span of iovec structures describing buffers. MUST remain valid until operation completes.
/// @param offset File offset to read from (default: 0)
/// @return Awaitable yielding Result<size_t> with total bytes read
///
/// @warning All iovec buffers and the span itself must remain valid until co_await returns!
///
/// @code
///   Header header;
///   std::array<std::byte, 1024> payload;
///   std::array<iovec, 2> vecs = {{
///       {&header, sizeof(header)},
///       {payload.data(), payload.size()}
///   }};
///   auto result = co_await AsyncReadv(ctx, fd, vecs, file_offset);
/// @endcode
template <FileDescriptor F>
ReadvOp AsyncReadv(IoContext& ctx, const F& f, std::span<const iovec> iovecs, uint64_t offset = 0)
{
    return ReadvOp(ctx, f, iovecs, offset);
}

struct WritevOp : UringOp
{
    int fd;
    std::span<const iovec> iovecs;
    uint64_t offset;

    template <FileDescriptor F>
    WritevOp(IoContext& ctx, const F& f, std::span<const iovec> iov, uint64_t off)
        : UringOp(&ctx), fd(GetRawFd(f)), iovecs(iov), offset(off)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) const
    {
        io_uring_prep_writev(sqe, fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset);
    }
};

/// @brief Writes data from multiple buffers (gather write).
/// @param ctx The IoContext to run on
/// @param f File descriptor to write to
/// @param iovecs Span of iovec structures describing buffers. MUST remain valid until operation completes.
/// @param offset File offset to write at (default: 0)
/// @return Awaitable yielding Result<size_t> with total bytes written
///
/// @warning All iovec buffers and the span itself must remain valid until co_await returns!
///
/// @code
///   Header header = make_header();
///   std::span<const std::byte> payload = get_payload();
///   std::array<iovec, 2> vecs = {{
///       {&header, sizeof(header)},
///       {payload.data(), payload.size()}
///   }};
///   co_await AsyncWritev(ctx, fd, vecs, file_offset);
/// @endcode
template <FileDescriptor F>
WritevOp AsyncWritev(IoContext& ctx, const F& f, std::span<const iovec> iovecs, uint64_t offset = 0)
{
    return WritevOp(ctx, f, iovecs, offset);
}

struct SendmsgOp : UringOp
{
    int fd;
    const msghdr* msg;
    unsigned flags;

    template <FileDescriptor F>
    SendmsgOp(IoContext& ctx, const F& f, const msghdr* m, unsigned fl)
        : UringOp(&ctx), fd(GetRawFd(f)), msg(m), flags(fl)
    {
    }

    void PrepareSqe(io_uring_sqe* sqe) { io_uring_prep_sendmsg(sqe, fd, msg, flags); }
};

/// @brief Sends a message with optional ancillary data (sendmsg).
/// @param ctx The IoContext to run on
/// @param f Socket to send to
/// @param msg Pointer to msghdr structure. MUST remain valid until operation completes.
/// @param flags Optional send flags (default: 0)
/// @return Awaitable yielding Result<size_t> with bytes sent
///
/// @warning The msghdr and all referenced buffers must remain valid until co_await returns!
///
/// @note Use for scatter-gather sends, sending to specific addresses (UDP),
///       or passing ancillary data (e.g., file descriptors via SCM_RIGHTS).
///
/// @code
///   msghdr msg{};
///   std::array<iovec, 2> iov = {{...}};
///   msg.msg_iov = iov.data();
///   msg.msg_iovlen = iov.size();
///   co_await AsyncSendmsg(ctx, socket, &msg);
/// @endcode
template <FileDescriptor F>
SendmsgOp AsyncSendmsg(IoContext& ctx, const F& f, const msghdr* msg, unsigned flags = 0)
{
    return SendmsgOp(ctx, f, msg, flags);
}

struct SleepOp : UringOp
{
    using UringOp::await_resume;

    __kernel_timespec ts{};

    template <typename Rep, typename Period>
    SleepOp(IoContext& ctx, std::chrono::duration<Rep, Period> dur) : UringOp(&ctx)
    {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        ts.tv_sec = ns / 1'000'000'000;
        ts.tv_nsec = ns % 1'000'000'000;
    }

    void PrepareSqe(io_uring_sqe* sqe) const { io_uring_prep_timeout(sqe, &ts, 0, 0); }

    Result<void> await_resume()
    {
        if (res == -ETIME)
            return {};
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return {};
    }
};

/// @brief Suspends the coroutine for the specified duration.
/// @param ctx The IoContext to run on
/// @param dur Duration to sleep (any std::chrono::duration type)
/// @return Awaitable yielding Result<void>
///
/// @note Uses io_uring timeout for efficient kernel-level sleep.
///       Does not block the thread - other coroutines continue running.
///
/// @code
///   using namespace std::chrono_literals;
///
///   // Retry with backoff
///   for (int i = 0; i < 3; ++i) {
///       auto result = co_await try_connect();
///       if (result) break;
///       co_await AsyncSleep(ctx, 100ms * (1 << i));  // 100ms, 200ms, 400ms
///   }
/// @endcode
template <typename Rep, typename Period>
SleepOp AsyncSleep(IoContext& ctx, std::chrono::duration<Rep, Period> dur)
{
    return SleepOp(ctx, dur);
}

}  // namespace aio
