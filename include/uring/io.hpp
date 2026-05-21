#pragma once

#include <chrono>
#include <filesystem>
#include <span>

#include <liburing.h>

#include "awaiter.hpp"
#include "fd.hpp"

namespace URing
{
/// @brief Accepts a new connection and populates the client's SocketAddress
[[nodiscard]] inline auto accept(Fd& server_fd, SocketAddress& client_addr, const int flags = 0)
{
    return IoAwaiter(
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
[[nodiscard]] inline auto accept(Fd& server_fd, const int flags = SOCK_NONBLOCK | SOCK_CLOEXEC)
{
    return IoAwaiter([raw_fd = server_fd.fd, flags](io_uring_sqe* sqe)
                     { io_uring_prep_accept(sqe, raw_fd, nullptr, nullptr, flags); },
                     [](const int32_t res) -> Result<Fd>
                     {
                         if (res < 0)
                             return std::unexpected(MakeErrorCode(res));
                         return Fd{res};
                     });
}

/// @brief Connects to a remote SocketAddress
[[nodiscard]] inline auto connect(Fd& fd, const SocketAddress& addr)
{
    // CRITICAL SAFETY FEATURE:
    // We capture 'addr' by VALUE inside the lambda. Because the lambda is
    // stored inside the IoAwaiter, and the IoAwaiter is pinned in the
    // coroutine frame, the kernel is guaranteed to read from a stable memory address
    // even if the caller's temporary SocketAddress goes out of scope!
    return IoAwaiter([raw_fd = fd.fd, addr](io_uring_sqe* sqe) mutable
                     { io_uring_prep_connect(sqe, raw_fd, addr.Get(), addr.addrlen); }, detail::ResumeVoid{});
}

/// Unified Read (offset = -1 tells io_uring to use the current file offset)
///
/// @warning The buffer pointed to by 'buf' MUST remain valid until the operation completes.
/// Do NOT pass a span to a temporary container (e.g., read(fd, std::vector<byte>(1024))).
[[nodiscard]] inline auto read(Fd& fd, std::span<std::byte> buf, off_t offset = -1)
{
    return IoAwaiter([raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
                     { io_uring_prep_read(sqe, raw_fd, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
}

/// @warning The iovecs and the buffers they point to MUST remain valid until completion.
[[nodiscard]] inline auto readv(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
{
    return IoAwaiter([raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
                     { io_uring_prep_readv(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
                     detail::ResumeInt{});
}

// Unified Write
///
/// @warning The buffer pointed to by 'buf' MUST remain valid until the operation completes.
[[nodiscard]] inline auto write(Fd& fd, std::span<const std::byte> buf, off_t offset = -1)
{
    return IoAwaiter([raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
                     { io_uring_prep_write(sqe, raw_fd, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
}

/// @warning The iovecs and the buffers they point to MUST remain valid until completion.
[[nodiscard]] inline auto writev(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
{
    return IoAwaiter(
        [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
        { io_uring_prep_writev(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
        detail::ResumeInt{});
}

// File Ops
[[nodiscard]] inline auto open(std::filesystem::path path, const int flags, const mode_t mode = 0644)
{
    return IoAwaiter([path, flags, mode](io_uring_sqe* sqe)
                     { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); },
                     [](const int32_t res) -> Result<Fd>
                     {
                         if (res < 0)
                             return std::unexpected(MakeErrorCode(res));
                         return Fd{res};
                     });
}

/// Close takes ownership of the FD on purpose.
/// Internally, it release the FD before performing an async close to avoid the Dtor of Fd to make a sync close.
[[nodiscard]] inline auto close(Fd&& fd)
{
    auto raw_fd = fd.Release();
    return IoAwaiter([raw_fd](io_uring_sqe* sqe) { io_uring_prep_close(sqe, raw_fd); }, detail::ResumeVoid{});
}

[[nodiscard]] inline auto remove(std::filesystem::path path)
{
    return IoAwaiter([path](io_uring_sqe* sqe) { io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0); },
                     detail::ResumeVoid{});
}

[[nodiscard]] inline auto fsync(Fd& fd, const bool full_sync = false)
{
    return IoAwaiter([raw_fd = fd.fd, full_sync](io_uring_sqe* sqe)
                     { io_uring_prep_fsync(sqe, raw_fd, full_sync ? 0u : IORING_FSYNC_DATASYNC); },
                     detail::ResumeVoid{});
}

[[nodiscard]] inline auto fallocate(Fd& fd, const int mode, const off_t offset, const off_t len)
{
    return IoAwaiter([raw_fd = fd.fd, mode, offset, len](io_uring_sqe* sqe)
                     { io_uring_prep_fallocate(sqe, raw_fd, mode, offset, len); }, detail::ResumeVoid{});
}

[[nodiscard]] inline auto ftruncate(Fd& fd, const off_t len)
{
    return IoAwaiter([raw_fd = fd.fd, len](io_uring_sqe* sqe) { io_uring_prep_ftruncate(sqe, raw_fd, len); },
                     detail::ResumeVoid{});
}

[[nodiscard]] inline auto poll(Fd& fd, const unsigned poll_mask)
{
    return IoAwaiter([raw_fd = fd.fd, poll_mask](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, raw_fd, poll_mask); },
                     detail::ResumeVoid{});
}

template <typename Rep, typename Period>
[[nodiscard]] auto timeout(const std::chrono::duration<Rep, Period> dur)
{
    return IoAwaiter(
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
[[nodiscard]] auto sleep(const std::chrono::duration<Rep, Period> dur)
{
    return timeout(dur);
}
}  // namespace URing
