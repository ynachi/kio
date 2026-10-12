#pragma once
#include "context.h"
#include "detail/awaitable.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>

#include <sys/socket.h>
#include <sys/uio.h>

#include "fd.hpp"

namespace kio::detail
{
    // The kernel caps a single read/write at MAX_RW_COUNT (0x7ffff000)
    // anyway; clamping turns a >4 GiB span into a short count the caller
    // can loop on, instead of a silently wrapped u32 length.
    inline constexpr std::size_t kMaxRwLen = 0x7ffff000;

    [[nodiscard]] constexpr unsigned clamp_len(const std::size_t n) noexcept
    {
        return static_cast<unsigned>(std::min(n, kMaxRwLen));
    }
} // namespace kio::detail

namespace kio::io
{
    inline auto async_read(Context& io, const UniqueFd& fd, std::span<std::byte> buf,
                           const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io,
            [raw_fd = fd.get(), buf, off](io_uring_sqe* sqe, __kernel_timespec*)
            {
                // ~0ULL means "current file position, then advance"
                io_uring_prep_read(sqe, raw_fd, buf.data(), detail::clamp_len(buf.size()), off);
            },
            false
        };
    }

    // File/pipe write. On a SOCKET this has write(2) SIGPIPE semantics: a
    // dead peer takes the whole process down. Use async_send for connected
    // sockets as it defaults to MSG_NOSIGNAL.
    inline auto async_write(Context& io, const UniqueFd& fd, std::span<const std::byte> buf,
                            const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), buf, off](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_write(sqe, raw_fd, buf.data(), detail::clamp_len(buf.size()), off);
            },
            false
        };
    }

    // Vectorized read: the span is captured by value, so the caller's iovec
    // array must stay alive until the operation completes
    inline auto async_readv(Context& io, const UniqueFd& fd, std::span<const iovec> iovecs,
                            const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), iovecs, off](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_readv(sqe, raw_fd, iovecs.data(), detail::clamp_len(iovecs.size()), off);
            },
            false
        };
    }

    inline auto async_writev(Context& io, const UniqueFd& fd, std::span<const iovec> iovecs,
                             const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), iovecs, off](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_writev(sqe, raw_fd, iovecs.data(), detail::clamp_len(iovecs.size()), off);
            },
            false
        };
    }

    inline auto async_recv(Context& io, const UniqueFd& fd, std::span<std::byte> buf, int flags = 0) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), buf, flags](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_recv(sqe, raw_fd, buf.data(), detail::clamp_len(buf.size()), flags);
            },
            false
        };
    }

    // Socket send. io_uring_prep_write() leaves sqe->msg_flags at 0, so
    // writing to a peer that has already disconnected raises SIGPIPE and
    // takes the whole process down. Means one dead client kills the server.
    // prep_send() carries msg_flags, so MSG_NOSIGNAL turns that into an
    // EPIPE completion the caller can handle
    inline auto async_send(Context& io, const UniqueFd& fd, std::span<const std::byte> buf,
                           const int flags = MSG_NOSIGNAL) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), buf, flags](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_send(sqe, raw_fd, buf.data(), detail::clamp_len(buf.size()), flags);
            },
            false
        };
    }

    inline auto async_accept(Context& io, const UniqueFd& fd, sockaddr* addr = nullptr, socklen_t* addrlen = nullptr,
                             const int flags = kDefaultAcceptFlags) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), addr, addrlen, flags](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_accept(sqe, raw_fd, addr, addrlen, flags);
            },
            false
        };
    }

    // Accept with the named address type. client_addr is captured BY
    // REFERENCE: the caller's SocketAddress must outlive the operation, and
    // is filled in by the kernel when the CQE lands.
    inline auto async_accept(Context& io, const UniqueFd& fd, SocketAddress& client_addr,
                             const int flags = kDefaultAcceptFlags) noexcept
    {
        return IoAwaitable{
            io,
            [raw_fd = fd.get(), &client_addr, flags](io_uring_sqe* sqe, __kernel_timespec*)
            {
                client_addr.addrlen = sizeof(sockaddr_storage);
                io_uring_prep_accept(sqe, raw_fd, client_addr.GetMutable(), &client_addr.addrlen, flags);
            },
            false
        };
    }

    inline auto async_connect(Context& io, const UniqueFd& fd, const sockaddr* addr, const socklen_t addrlen) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), addr, addrlen](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_connect(sqe, raw_fd, addr, addrlen);
            },
            false
        };
    }

    inline auto async_connect(Context& io, const UniqueFd& fd, const SocketAddress& addr) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), &addr](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_connect(sqe, raw_fd, addr.Get(), addr.addrlen);
            },
            false
        };
    }

    // The path is moved into the awaiter's lambda, which outlives the call
    // expression
    inline auto async_open(Context& io, std::filesystem::path path, const int flags, const mode_t mode = 0644) noexcept
    {
        return IoAwaitable{
            io, [path = std::move(path), flags, mode](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode);
            },
            false
        };
    }

    // Consumes the UniqueFd (requires caller to use std::move) so the kernel
    // can safely close it without the destructor double-closing it. The fd
    // is released at SUBMIT time, not call time: an awaiter destroyed before
    // the co_await still closes the fd through ~UniqueFd instead of leaking
    // a released raw fd.
    inline auto async_close(Context& io, UniqueFd fd) noexcept
    {
        return IoAwaitable{
            io, [fd = std::move(fd)](io_uring_sqe* sqe, __kernel_timespec*) mutable
            {
                io_uring_prep_close(sqe, fd.release());
            },
            false
        };
    }

    inline auto async_remove(Context& io, std::filesystem::path path) noexcept
    {
        return IoAwaitable{
            io, [path = std::move(path)](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0);
            },
            false
        };
    }

    inline auto async_rename(Context& io, std::filesystem::path from, std::filesystem::path to) noexcept
    {
        return IoAwaitable{
            io, [from = std::move(from), to = std::move(to)](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0);
            },
            false
        };
    }

    // full_sync = false maps to IORING_FSYNC_DATASYNC (the cheaper fdatasync
    // semantics); true asks for a full fsync.
    inline auto async_fsync(Context& io, const UniqueFd& fd, const bool full_sync = false) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), full_sync](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_fsync(sqe, raw_fd, full_sync ? 0u : IORING_FSYNC_DATASYNC);
            },
            false
        };
    }

    inline auto async_fallocate(Context& io, const UniqueFd& fd, const int mode, const off_t offset,
                                const off_t len) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), mode, offset, len](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_fallocate(sqe, raw_fd, mode, static_cast<__u64>(offset), static_cast<__u64>(len));
            },
            false
        };
    }

    // Needs liburing >= 2.9 for io_uring_prep_ftruncate; this tree pins 2.9.
    inline auto async_ftruncate(Context& io, const UniqueFd& fd, const off_t len) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), len](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_ftruncate(sqe, raw_fd, len);
            },
            false
        };
    }

    // The CQE carries the returned poll mask bits as a non-negative int.
    inline auto async_poll(Context& io, const UniqueFd& fd, const unsigned poll_mask) noexcept
    {
        return IoAwaitable{
            io, [raw_fd = fd.get(), poll_mask](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_poll_add(sqe, raw_fd, poll_mask);
            },
            false
        };
    }

    inline auto async_sleep(Context& io, std::chrono::nanoseconds d) noexcept
    {
        return IoAwaitable{
            io,
            [d](io_uring_sqe* sqe, __kernel_timespec* ts)
            {
                // Negative means "already elapsed"
                const auto dur = d < std::chrono::nanoseconds{} ? std::chrono::nanoseconds{} : d;
                const auto sec = std::chrono::duration_cast<std::chrono::seconds>(dur);
                ts->tv_sec = sec.count();
                ts->tv_nsec = (dur - sec).count();
                io_uring_prep_timeout(sqe, ts, 0, 0);
            },
            /*etime_is_success=*/true
        };
    }

    inline auto async_read_fixed(Context& io, const UniqueFd& fd, BufferLease& buf, const size_t len,
                                 const size_t buffer_offset,
                                 const std::uint64_t off) noexcept
    {
        CHECK(io.buffer_pool().has_value() && buf.pool() == &*io.buffer_pool())
      << "async_write_fixed: empty or foreign lease (not registered with this Context)";
        const bool bad_range = !(buffer_offset < buf.size() && len > 0);
        const size_t safe_len = bad_range ? 0 : std::min({len, buf.size() - buffer_offset, detail::kMaxRwLen});
        std::byte* ptr = buf.data() + (bad_range ? 0 : buffer_offset);
        const int buf_index = buf.pool()->registration_index(ptr);
        const int raw_fd = fd.get();
        return IoAwaitable{
            io,
            [raw_fd, ptr, safe_len, off, buf_index](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_read_fixed(sqe, raw_fd, ptr, static_cast<unsigned>(safe_len), off, buf_index);
            },
            false,
            bad_range ? EINVAL : 0
        };
    }

    inline auto async_write_fixed(Context& io, const UniqueFd& fd, const BufferLease& buf, const size_t len,
                                  const size_t buffer_offset, const std::uint64_t off) noexcept
    {
        CHECK(io.buffer_pool().has_value() && buf.pool() == &*io.buffer_pool())
      << "async_write_fixed: empty or foreign lease (not registered with this Context)";
        const bool bad_range = !(buffer_offset < buf.size() && len > 0);
        const size_t safe_len = std::min({len, buf.size() - buffer_offset, detail::kMaxRwLen});
        const std::byte* ptr = buf.data() + buffer_offset;
        const int buf_index = buf.pool()->registration_index(ptr);
        const int raw_fd = fd.get();
        return IoAwaitable{
            io, [raw_fd, ptr, safe_len, off, buf_index](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_write_fixed(sqe, raw_fd, ptr, static_cast<unsigned>(safe_len), off, buf_index);
            },
            false,
            bad_range ? EINVAL : 0
        };
    }


    inline void cancel(Context& io, IoAwaitableBase& op) noexcept
    {
        io.cancel(op);
    }
} // namespace kio::io
