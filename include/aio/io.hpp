#pragma once

#include <concepts>
#include <cstddef>
#include <span>

#include "IoContext.hpp"

namespace aio
{

// Forward declarations
class IoContext;
template <typename Derived>
struct UringOp;

// -----------------------------------------------------------------------------
// Concepts & Helpers
// -----------------------------------------------------------------------------

// Matches int, or any type with a .get() -> int method (like Socket)
template <typename T>
concept FileDescriptor = std::convertible_to<T, int> || requires(const T& t) {
    { t.get() } -> std::convertible_to<int>;
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
        return fd.get();
    }
}

struct AcceptOp : UringOp<AcceptOp>
{
    using UringOp::await_resume;

    int fd;
    sockaddr_storage addr{};
    socklen_t addrlen = sizeof(addr);

    template <FileDescriptor F>
    AcceptOp(IoContext& ctx, const F& f) : UringOp(&ctx), fd(GetRawFd(f)) {}

    void prepare_sqe(io_uring_sqe* sqe)
    {
        io_uring_prep_accept(sqe, fd, reinterpret_cast<sockaddr*>(&addr), &addrlen, 0);
    }

    Result<int> await_resume()
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return res;
    }
};

template <FileDescriptor F>
AcceptOp AsyncAccept(IoContext& ctx, const F& f)
{
    return AcceptOp(ctx, f);
}

struct RecvOp : UringOp<RecvOp>
{
    int fd;
    std::span<std::byte> buffer;
    int flags;

    template <FileDescriptor F>
    RecvOp(IoContext& ctx, const F& f, std::span<std::byte> buf, int flags)
        : UringOp(&ctx), fd(GetRawFd(f)), buffer(buf), flags(flags)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_recv(sqe, fd, buffer.data(), buffer.size(), flags); }
};

// Primary API: std::span<std::byte>
template <FileDescriptor F>
RecvOp AsyncRecv(IoContext& ctx, const F& f, std::span<std::byte> buffer, int flags = 0)
{
    return RecvOp{ctx, f, buffer, flags};
}

// Convenience: char array
template <FileDescriptor F, size_t N>
RecvOp AsyncRecv(IoContext& ctx, const F& f, char (&buf)[N], int flags = 0)
{
    return RecvOp{
        ctx, f, std::span{reinterpret_cast<std::byte*>(buf), N},
          flags
    };
}

// Convenience: std::array
template <FileDescriptor F, size_t N>
RecvOp AsyncRecv(IoContext& ctx, const F& f, std::array<std::byte, N>& buf, int flags = 0)
{
    return RecvOp{ctx, f, std::span{buf}, flags};
}

struct SendOp : UringOp<SendOp>
{
    int fd;
    std::span<const std::byte> buffer;
    int flags;

    template <FileDescriptor F>
    SendOp(IoContext& ctx, const F& f, std::span<const std::byte> buf, int flags)
        : UringOp(&ctx), fd(GetRawFd(f)), buffer(buf), flags(flags)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_send(sqe, fd, buffer.data(), buffer.size(), flags); }
};

// Primary API
template <FileDescriptor F>
inline SendOp AsyncSend(IoContext& ctx, const F& f, std::span<const std::byte> buffer, int flags = 0)
{
    return SendOp{ctx, f, buffer, flags};
}

// Convenience: string_view → perfect for HTTP responses
inline SendOp AsyncSend(IoContext& ctx, int fd, std::string_view str, int flags = 0)
{
    return SendOp{
        ctx, fd, std::span{reinterpret_cast<const std::byte*>(str.data()), str.size()},
          flags
    };
}

// Convenience: const char array
template <size_t N>
SendOp AsyncSend(IoContext& ctx, int fd, const char (&buf)[N], int flags = 0)
{
    return SendOp{
        ctx, fd, std::span{reinterpret_cast<const std::byte*>(buf), N - 1}, // Skip null terminator
        flags
    };
}

struct ReadOp : UringOp<ReadOp>
{
    int fd;
    std::span<std::byte> buffer;
    uint64_t offset;

    ReadOp(IoContext& ctx, int fd, std::span<std::byte> buf, uint64_t off)
        : UringOp(&ctx), fd(fd), buffer(buf), offset(off)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, buffer.data(), buffer.size(), offset); }
};

inline ReadOp AsyncRead(IoContext& ctx, int fd, std::span<std::byte> buffer, uint64_t offset = 0)
{
    return ReadOp{ctx, fd, buffer, offset};
}

struct WriteOp : UringOp<WriteOp>
{
    int fd;
    std::span<const std::byte> buffer;
    uint64_t offset;

    WriteOp(IoContext& ctx, int fd, std::span<const std::byte> buf, uint64_t off)
        : UringOp(&ctx), fd(fd), buffer(buf), offset(off)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_write(sqe, fd, buffer.data(), buffer.size(), offset); }
};

inline WriteOp AsyncWrite(IoContext& ctx, int fd, std::span<const std::byte> buffer, uint64_t offset = 0)
{
    return WriteOp{ctx, fd, buffer, offset};
}

struct CloseOp : UringOp<CloseOp>
{
    using UringOp::await_resume;

    int fd;

    CloseOp(IoContext& ctx, int fd) : UringOp(&ctx), fd(fd) {}

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd); }

    Result<void> await_resume()
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return {};
    }
};

struct ReadFixedOp : UringOp<ReadFixedOp>
{
    int file_index;
    void* buffer;
    size_t len;
    off_t offset;

    ReadFixedOp(IoContext& ctx, int idx, std::span<std::byte> buf, off_t off)
        : UringOp(&ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe)
    {
        io_uring_prep_read(sqe, file_index, buffer, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    }
};

inline ReadFixedOp AsyncReadFixed(IoContext& ctx, int idx, std::span<std::byte> buf, off_t off)
{
    return ReadFixedOp(ctx, idx, buf, off);
}

struct WriteFixedOp : UringOp<WriteFixedOp>
{
    int file_index;
    const void* buffer;
    size_t len;
    off_t offset;

    WriteFixedOp(IoContext& ctx, int idx, std::span<const std::byte> buf, off_t off)
        : UringOp(&ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe)
    {
        io_uring_prep_write(sqe, file_index, buffer, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    }
};

inline WriteFixedOp AsyncWriteFixed(IoContext& ctx, int idx, std::span<const std::byte> buf, off_t off)
{
    return WriteFixedOp(ctx, idx, buf, off);
}

inline CloseOp AsyncClose(IoContext& ctx, int fd)
{
    return CloseOp(ctx, fd);
}

struct ConnectOp : UringOp<ConnectOp>
{
    using UringOp::await_resume;

    int fd;
    sockaddr_storage addr_store{};
    socklen_t addrlen;

    ConnectOp(IoContext& ctx, int fd, const sockaddr* addr, socklen_t len) : UringOp(&ctx), fd(fd), addrlen(len)
    {
        std::memcpy(&addr_store, addr, len);
    }

    void prepare_sqe(io_uring_sqe* sqe)
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

inline ConnectOp AsyncConnect(IoContext& ctx, int fd, const sockaddr* addr, socklen_t len)
{
    return ConnectOp(ctx, fd, addr, len);
}

struct SleepOp : UringOp<SleepOp>
{
    using UringOp::await_resume;

    __kernel_timespec ts;

    template <typename Rep, typename Period>
    SleepOp(IoContext& ctx, std::chrono::duration<Rep, Period> dur) : UringOp(&ctx)
    {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        ts.tv_sec = ns / 1'000'000'000;
        ts.tv_nsec = ns % 1'000'000'000;
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_timeout(sqe, &ts, 0, 0); }

    Result<void> await_resume()
    {
        if (res == -ETIME)
            return {};
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return {};
    }
};

template <typename Rep, typename Period>
SleepOp AsyncSleep(IoContext& ctx, std::chrono::duration<Rep, Period> dur)
{
    return SleepOp(ctx, dur);
}

}  // namespace aio