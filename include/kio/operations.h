#pragma once
#include "context.h"
#include "fd.hpp"
#include "detail/awaitable.h"

// The etime_is_success flag is folded into IoAwaitableBase::state (a spare
// bit), so it no longer costs a byte per awaiter -- but every non-timeout op
// still pays the -ETIME branch in await_resume. A compile-time split
// (TimeSuccessOp tag) would remove that too if it ever shows in a profile.

namespace kio::io
{
    inline auto async_read(Context& io, UniqueFd& fd, void* buf, const unsigned len,
                           const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                // ~0ULL means "current file position, then advance" -- same
                // default as async_write, so sequential reads work.
                io_uring_prep_read(sqe, fd.get(), buf, len, off);
            },
            false
        };
    }

    inline auto async_write(Context& io, UniqueFd& fd, const void* buf, unsigned len,
                            const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_write(sqe, fd.get(), buf, len, off);
            },
            false
        };
    }

    inline auto async_recv(Context& io, UniqueFd& fd, void* buf, std::size_t len, int flags = 0) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_recv(sqe, fd.get(), buf, len, flags);
            },
            false
        };
    }

    inline auto async_send(Context& io, UniqueFd& fd, const void* buf, std::size_t len, int flags = 0) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_send(sqe, fd.get(), buf, len, flags);
            },
            false
        };
    }

    inline auto async_accept(Context& io, UniqueFd& fd, sockaddr* addr = nullptr, socklen_t* addrlen = nullptr,

                             const int flags = 0
    )
        noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_accept(sqe, fd.get(), addr, addrlen, flags);
            },
            false
        };
    }

    inline auto async_connect(Context& io, UniqueFd& fd, const sockaddr* addr, const socklen_t addrlen) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_connect(sqe, fd.get(), addr, addrlen);
            },
            false
        };
    }

    // Consumes the UniqueFd (requires caller to use std::move) so the kernel
    // can safely close it without the destructor double-closing it.
    inline auto async_close(Context& io, UniqueFd fd) noexcept
    {
        int raw_fd = fd.release();
        return IoAwaitable{
            io,
            [raw_fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_close(sqe, raw_fd);
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
                const auto sec = std::chrono::duration_cast<std::chrono::seconds>(d);
                ts->tv_sec = sec.count();
                ts->tv_nsec = (d - sec).count();
                io_uring_prep_timeout(sqe, ts, 0, 0);
            },
            /*etime_is_success=*/true
        };
    }

    inline void cancel(Context& io, IoAwaitableBase& op) noexcept
    {
        io.cancel(op);
    }
} // namespace kio::io
