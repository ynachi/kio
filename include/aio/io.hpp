#pragma once
// aio/io_operations_v2.hpp
// Proposed modernized I/O operations using std::span
//
// Changes from the current implementation:
// 1. std::span<std::byte> for all buffer operations
// 2. io_context& instead of io_context* where non-null
// 3. Convenience overloads for common patterns

#include <concepts>
#include <cstddef>
#include <span>

#include "io_context.hpp"

namespace aio
{

// Forward declarations
class io_context;
template <typename Derived>
struct uring_op;

// -----------------------------------------------------------------------------
// Concepts & Helpers
// -----------------------------------------------------------------------------

// Matches int, or any type with a .get() -> int method (like Socket)
template <typename T>
concept FileDescriptor = std::convertible_to<T, int> || requires(const T& t) {
    { t.get() } -> std::convertible_to<int>;
};

// Helper to extract the raw fd
constexpr int get_raw_fd(const FileDescriptor auto& fd)
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

struct RecvOp : uring_op<RecvOp>
{
    int fd;
    std::span<std::byte> buffer;
    int flags;

    template <FileDescriptor F>
    RecvOp(io_context& ctx, const F& f, std::span<std::byte> buf, int flags)
        : uring_op(&ctx), fd(get_raw_fd(f)), buffer(buf), flags(flags)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_recv(sqe, fd, buffer.data(), buffer.size(), flags); }
};

// Primary API: std::span<std::byte>
template <FileDescriptor F>
inline RecvOp async_recv(io_context& ctx, const F& f, std::span<std::byte> buffer, int flags = 0)
{
    return RecvOp{ctx, f, buffer, flags};
}

// Convenience: char array
template <FileDescriptor F, size_t N>
inline RecvOp async_recv(io_context& ctx, const F& f, char (&buf)[N], int flags = 0)
{
    return RecvOp{
        ctx, f, std::span{reinterpret_cast<std::byte*>(buf), N},
          flags
    };
}

// Convenience: std::array
template <FileDescriptor F, size_t N>
inline RecvOp async_recv(io_context& ctx, const F& f, std::array<std::byte, N>& buf, int flags = 0)
{
    return RecvOp{ctx, f, std::span{buf}, flags};
}

struct SendOp : uring_op<SendOp>
{
    int fd;
    std::span<const std::byte> buffer;
    int flags;

    SendOp(io_context& ctx, int fd, std::span<const std::byte> buf, int flags)
        : uring_op(&ctx), fd(fd), buffer(buf), flags(flags)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_send(sqe, fd, buffer.data(), buffer.size(), flags); }
};

// Primary API
inline SendOp async_send(io_context& ctx, int fd, std::span<const std::byte> buffer, int flags = 0)
{
    return SendOp{ctx, fd, buffer, flags};
}

// Convenience: string_view → perfect for HTTP responses
inline SendOp async_send(io_context& ctx, int fd, std::string_view str, int flags = 0)
{
    return SendOp{
        ctx, fd, std::span{reinterpret_cast<const std::byte*>(str.data()), str.size()},
          flags
    };
}

// Convenience: const char array
template <size_t N>
SendOp async_send(io_context& ctx, int fd, const char (&buf)[N], int flags = 0)
{
    return SendOp{
        ctx, fd, std::span{reinterpret_cast<const std::byte*>(buf), N - 1}, // Skip null terminator
        flags
    };
}

struct ReadOp : uring_op<ReadOp>
{
    int fd;
    std::span<std::byte> buffer;
    uint64_t offset;

    ReadOp(io_context& ctx, int fd, std::span<std::byte> buf, uint64_t off)
        : uring_op(&ctx), fd(fd), buffer(buf), offset(off)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, buffer.data(), buffer.size(), offset); }
};

inline ReadOp async_read(io_context& ctx, int fd, std::span<std::byte> buffer, uint64_t offset = 0)
{
    return ReadOp{ctx, fd, buffer, offset};
}

struct WriteOp : uring_op<WriteOp>
{
    int fd;
    std::span<const std::byte> buffer;
    uint64_t offset;

    WriteOp(io_context& ctx, int fd, std::span<const std::byte> buf, uint64_t off)
        : uring_op(&ctx), fd(fd), buffer(buf), offset(off)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_write(sqe, fd, buffer.data(), buffer.size(), offset); }
};

inline WriteOp async_write(io_context& ctx, int fd, std::span<const std::byte> buffer, uint64_t offset = 0)
{
    return WriteOp{ctx, fd, buffer, offset};
}

struct CloseOp : uring_op<CloseOp>
{
    using uring_op::await_resume;

    int fd;

    CloseOp(io_context& ctx, int fd) : uring_op(&ctx), fd(fd) {}

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd); }

    Result<void> await_resume()
    {
        if (res < 0)
            return std::unexpected(make_error_code(res));
        return {};
    }
};

struct ReadFixedOp : uring_op<ReadFixedOp>
{
    int file_index;
    void* buffer;
    size_t len;
    off_t offset;

    ReadFixedOp(io_context& ctx, int idx, std::span<std::byte> buf, off_t off)
        : uring_op(&ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe)
    {
        io_uring_prep_read(sqe, file_index, buffer, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    }
};

inline ReadFixedOp async_read_fixed(io_context& ctx, int idx, std::span<std::byte> buf, off_t off)
{
    return ReadFixedOp(ctx, idx, buf, off);
}

struct WriteFixedOp : uring_op<WriteFixedOp>
{
    int file_index;
    const void* buffer;
    size_t len;
    off_t offset;

    WriteFixedOp(io_context& ctx, int idx, std::span<const std::byte> buf, off_t off)
        : uring_op(&ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe)
    {
        io_uring_prep_write(sqe, file_index, buffer, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    }
};

inline WriteFixedOp async_write_fixed(io_context& ctx, int idx, std::span<const std::byte> buf, off_t off)
{
    return WriteFixedOp(ctx, idx, buf, off);
}

inline CloseOp async_close(io_context& ctx, int fd)
{
    return CloseOp(ctx, fd);
}

struct ConnectOp : uring_op<ConnectOp>
{
    using uring_op::await_resume;

    int fd;
    sockaddr_storage addr_store{};
    socklen_t addrlen;

    ConnectOp(io_context& ctx, int fd, const sockaddr* addr, socklen_t len) : uring_op(&ctx), fd(fd), addrlen(len)
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
            return std::unexpected(make_error_code(res));
        return {};
    }
};

inline ConnectOp async_connect(io_context& ctx, int fd, const sockaddr* addr, socklen_t len)
{
    return ConnectOp(ctx, fd, addr, len);
}

struct AcceptOp : uring_op<AcceptOp>
{
    using uring_op::await_resume;

    int fd;
    sockaddr_storage addr{};
    socklen_t addrlen = sizeof(addr);

    AcceptOp(io_context& ctx, int fd) : uring_op(&ctx), fd(fd) {}

    void prepare_sqe(io_uring_sqe* sqe)
    {
        io_uring_prep_accept(sqe, fd, reinterpret_cast<sockaddr*>(&addr), &addrlen, 0);
    }

    Result<int> await_resume()
    {
        if (res < 0)
            return std::unexpected(make_error_code(res));
        return res;
    }
};

inline AcceptOp async_accept(io_context& ctx, int fd)
{
    return AcceptOp(ctx, fd);
}

struct SleepOp : uring_op<SleepOp>
{
    using uring_op::await_resume;

    __kernel_timespec ts;

    template <typename Rep, typename Period>
    SleepOp(io_context& ctx, std::chrono::duration<Rep, Period> dur) : uring_op(&ctx)
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
            return std::unexpected(make_error_code(res));
        return {};
    }
};

template <typename Rep, typename Period>
SleepOp async_sleep(io_context& ctx, std::chrono::duration<Rep, Period> dur)
{
    return SleepOp(ctx, dur);
}

// =============================================================================
// Usage Examples (compile-time verification)
// =============================================================================

namespace examples
{

task<void> demo_buffer_safety(io_context& ctx, int fd)
{
    // Stack buffer with automatic size deduction
    char buf[4096];
    auto result = co_await async_recv(ctx, fd, buf);  // No size needed!

    // std::array (recommended for modern code)
    std::array<std::byte, 4096> buffer;
    auto r2 = co_await async_recv(ctx, fd, buffer);

    // Explicit span (when you need subranges)
    auto r3 = co_await async_recv(ctx, fd, std::span{buffer}.subspan(0, 1024));

    // String view for sends (zero copy)
    co_await async_send(ctx, fd, "HTTP/1.1 200 OK\r\n");

    // Compile error: size mismatch impossible!
    // co_await async_recv(ctx, fd, buffer, 8192);  // Won't compile!
}

}  // namespace examples

}  // namespace aio