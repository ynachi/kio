#pragma once
#include "context.h"

#include <chrono>
#include <filesystem>
#include <span>

#include <liburing.h>

#include "awaiter.hpp"
#include "fd.hpp"

#if URING_ENABLE_TRACING
    #define URING_TRACE_OP_NAME(name) name,
#else
    #define URING_TRACE_OP_NAME(name)
#endif

namespace URing
{
/// @brief Accepts a new connection and populates the client's SocketAddress
inline auto accept(IoContext& ctx, Fd& server_fd, SocketAddress& client_addr, const int flags = 0)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("accept")
        [raw_fd = server_fd.fd, &client_addr, flags](io_uring_sqe* sqe)
        {
            // Pre-fill the addrlen so the kernel knows the max buffer size
            client_addr.addrlen = sizeof(sockaddr_storage);
            io_uring_prep_accept(sqe, raw_fd, client_addr.GetMutable(), &client_addr.addrlen, flags);
        },
        [](const int32_t res) -> Result<Fd>
        {
            if (res < 0)
                return std::unexpected(MakeErrorCode(res));
            // Wrap the newly accepted raw FD into our RAII struct immediately
            return Fd{res};
        });
}

/// @brief Accepts a new connection without capturing the client's address
/// Accept default flag is set to SOCK_NONBLOCK | SOCK_CLOEXEC
inline auto accept(IoContext& ctx, Fd& server_fd, const int flags = SOCK_NONBLOCK | SOCK_CLOEXEC)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("accept") [raw_fd = server_fd.fd, flags](io_uring_sqe* sqe)
        { io_uring_prep_accept(sqe, raw_fd, nullptr, nullptr, flags); },
        [](const int32_t res) -> Result<Fd>
        {
            if (res < 0)
                return std::unexpected(MakeErrorCode(res));
            return Fd{res};
        });
}

/// @brief Connects to a remote SocketAddress
inline auto connect(IoContext& ctx, Fd& fd, const SocketAddress& addr)
{
    // CRITICAL SAFETY FEATURE:
    // We capture 'addr' by VALUE inside the lambda. Because the lambda is
    // stored inside the IoAwaiter, and the IoAwaiter is pinned in the
    // coroutine frame, the kernel is guaranteed to read from a stable memory address
    // even if the caller's temporary SocketAddress goes out of scope!
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("connect") [raw_fd = fd.fd, addr](io_uring_sqe* sqe) mutable
        { io_uring_prep_connect(sqe, raw_fd, addr.Get(), addr.addrlen); }, detail::ResumeVoid{});
}

/// Unified Read (offset = -1 tells io_uring to use the current file offset)
inline auto read(IoContext& ctx, Fd& fd, std::span<std::byte> buf, off_t offset = -1)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("read") [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
        { io_uring_prep_read(sqe, raw_fd, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
}

inline auto readv(IoContext& ctx, Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("readv") [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
        { io_uring_prep_readv(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
        detail::ResumeInt{});
}

// Unified Write
inline auto write(IoContext& ctx, Fd& fd, std::span<const std::byte> buf, off_t offset = -1)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("write") [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
        { io_uring_prep_write(sqe, raw_fd, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
}

inline auto writev(IoContext& ctx, Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("writev") [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
        { io_uring_prep_writev(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
        detail::ResumeInt{});
}

// File Ops
inline auto open(IoContext& ctx, std::filesystem::path path, const int flags, const mode_t mode = 0644)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("open") [path, flags, mode](io_uring_sqe* sqe) { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); },
        [](const int32_t res) -> Result<Fd>
        {
            if (res < 0)
                return std::unexpected(MakeErrorCode(res));
            return Fd{res};
        });
}

inline auto remove(IoContext& ctx, std::filesystem::path path)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("unlink") [path](io_uring_sqe* sqe) { io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0); },
        detail::ResumeVoid{});
}

inline auto fsync(IoContext& ctx, Fd& fd, const bool full_sync = false)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("fsync") [raw_fd = fd.fd, full_sync](io_uring_sqe* sqe)
        { io_uring_prep_fsync(sqe, raw_fd, full_sync ? 0u : IORING_FSYNC_DATASYNC); }, detail::ResumeVoid{});
}

inline auto fallocate(IoContext& ctx, Fd& fd, const int mode, const off_t offset, const off_t len)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("fallocate") [raw_fd = fd.fd, mode, offset, len](io_uring_sqe* sqe)
        { io_uring_prep_fallocate(sqe, raw_fd, mode, offset, len); }, detail::ResumeVoid{});
}

inline auto ftruncate(IoContext& ctx, Fd& fd, const off_t len)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("ftruncate") [raw_fd = fd.fd, len](io_uring_sqe* sqe) { io_uring_prep_ftruncate(sqe, raw_fd, len); },
        detail::ResumeVoid{});
}

inline auto poll(IoContext& ctx, Fd& fd, const unsigned poll_mask)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("poll") [raw_fd = fd.fd, poll_mask](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, raw_fd, poll_mask); },
        detail::ResumeVoid{});
}

// The New Timeout (Notice the 'mutable' lambda so we can take the address of ts)
template <typename Rep, typename Period>
auto timeout(IoContext& ctx, const std::chrono::duration<Rep, Period> dur)
{
    return IoAwaiter(
        ctx, URING_TRACE_OP_NAME("timeout")
        [ts = __kernel_timespec{.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(dur).count(),
                                .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count() %
                                           1'000'000'000}](io_uring_sqe* sqe) mutable
        { io_uring_prep_timeout(sqe, &ts, 0, 0); },
        [](const int32_t res) -> Result<void>
        {
            if (res == -ETIME || res == 0)
                return {};
            return std::unexpected(MakeErrorCode(res));
        });
}

template <typename Rep, typename Period>
auto sleep(IoContext& ctx, const std::chrono::duration<Rep, Period> dur)
{
    return timeout(ctx, dur);
}
}  // namespace URing

#undef URING_TRACE_OP_NAME
