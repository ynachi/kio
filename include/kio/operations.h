#pragma once
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>

#include <sys/uio.h> // iovec

#include "context.h"
#include "fd.hpp"
#include "detail/awaitable.h"

// The etime_is_success flag is folded into IoAwaitableBase::state (a spare
// bit), so it no longer costs a byte per awaiter -- but every non-timeout op
// still pays the -ETIME branch in await_resume. A compile-time split
// (TimeSuccessOp tag) would remove that too if it ever shows in a profile.
//
// Every operation below has the same shape: an IoAwaitable whose setup
// lambda fills the SQE. Two rules apply to all of them:
//
// 1. CAPTURE BY VALUE, OR BY REFERENCE-TO-OWNER -- NEVER A TEMPORARY. The
//    lambda lives in the coroutine frame for as long as the operation is
//    outstanding, which is longer than the full expression that created it.
//    raw_fd (an int) and spans are cheap by-value captures; paths are moved
//    in. The two that capture references (accept's client_addr, connect's
//    addr) name it in their comments: the caller's object MUST outlive the
//    operation.
//
// 2. ONE RESULT TYPE. The v2 awaiter resumes Result<int> for every op: byte
//    counts for I/O, the raw fd for open/accept (wrap in UniqueFd at the
//    call site), 0 for void ops (close/remove/rename/fsync/fallocate/
//    ftruncate), the poll mask bits for poll.
//
// [[nodiscard]] on all of them is deliberate and not decorative: the
// returned awaiter is where the result lives, so discarding it silently
// drops an I/O error.

namespace kio::io
{
    inline auto async_read(Context& io, UniqueFd& fd, std::span<std::byte> buf,
                           const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                // ~0ULL means "current file position, then advance" -- same
                // default as async_write, so sequential reads work.
                io_uring_prep_read(sqe, fd.get(), buf.data(), static_cast<unsigned>(buf.size()), off);
            },
            false
        };
    }

    inline auto async_write(Context& io, UniqueFd& fd, std::span<const std::byte> buf,
                            const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_write(sqe, fd.get(), buf.data(), static_cast<unsigned>(buf.size()), off);
            },
            false
        };
    }

    // Vectorized read: the span is captured by value, so the caller's iovec
    // array must stay alive until the operation completes -- same rule as
    // any buffer passed to an async call.
    inline auto async_readv(Context& io, UniqueFd& fd, std::span<const iovec> iovecs,
                            const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_readv(sqe, fd.get(), iovecs.data(), static_cast<unsigned>(iovecs.size()), off);
            },
            false
        };
    }

    inline auto async_writev(Context& io, UniqueFd& fd, std::span<const iovec> iovecs,
                             const std::uint64_t off = ~0ULL) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_writev(sqe, fd.get(), iovecs.data(), static_cast<unsigned>(iovecs.size()),
                                     off);
            },
            false
        };
    }

    inline auto async_recv(Context& io, UniqueFd& fd, std::span<std::byte> buf, int flags = 0) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_recv(sqe, fd.get(), buf.data(), buf.size(), flags);
            },
            false
        };
    }

    // Socket send. io_uring_prep_write() leaves sqe->msg_flags at 0, so
    // writing to a peer that has already disconnected raises SIGPIPE and
    // takes the whole process down -- one dead client kills the server.
    // prep_send() carries msg_flags, so MSG_NOSIGNAL turns that into an
    // EPIPE completion the caller can handle. No offset: sockets are not
    // written positionally.
    inline auto async_send(Context& io, UniqueFd& fd, std::span<const std::byte> buf,
                           const int flags = 0) noexcept
    {
        return IoAwaitable{
            io,
            [=, &fd](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_send(sqe, fd.get(), buf.data(), buf.size(), flags);
            },
            false
        };
    }

    inline auto async_accept(Context& io, UniqueFd& fd, sockaddr* addr = nullptr,
                             socklen_t* addrlen = nullptr, const int flags = 0
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

    // Accept with the named address type. client_addr is captured BY
    // REFERENCE: the caller's SocketAddress must outlive the operation, and
    // is filled in by the kernel when the CQE lands.
    inline auto async_accept(Context& io, UniqueFd& fd, SocketAddress& client_addr,
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

    inline auto async_connect(Context& io, UniqueFd& fd, const sockaddr* addr,
                              const socklen_t addrlen) noexcept
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

    // addr is captured BY REFERENCE and MUST outlive the operation.
    inline auto async_connect(Context& io, UniqueFd& fd, const SocketAddress& addr) noexcept
    {
        return IoAwaitable{
            io,
            [raw_fd = fd.get(), &addr](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_connect(sqe, raw_fd, addr.Get(), addr.addrlen);
            },
            false
        };
    }

    // The path is moved into the awaiter's lambda, which outlives the call
    // expression -- the c_str() the kernel reads at submit time stays valid.
    // Returns the raw fd: wrap with UniqueFd{*res} at the call site.
    inline auto async_open(Context& io, std::filesystem::path path, const int flags,
                           const mode_t mode = 0644) noexcept
    {
        return IoAwaitable{
            io,
            [path = std::move(path), flags, mode](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode);
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

    inline auto async_remove(Context& io, std::filesystem::path path) noexcept
    {
        return IoAwaitable{
            io,
            [path = std::move(path)](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0);
            },
            false
        };
    }

    inline auto async_rename(Context& io, std::filesystem::path from,
                             std::filesystem::path to) noexcept
    {
        return IoAwaitable{
            io,
            [from = std::move(from), to = std::move(to)](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0);
            },
            false
        };
    }

    // full_sync = false maps to IORING_FSYNC_DATASYNC (the cheaper fdatasync
    // semantics); true asks for a full fsync.
    inline auto async_fsync(Context& io, UniqueFd& fd, const bool full_sync = false) noexcept
    {
        return IoAwaitable{
            io,
            [raw_fd = fd.get(), full_sync](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_fsync(sqe, raw_fd, full_sync ? 0u : IORING_FSYNC_DATASYNC);
            },
            false
        };
    }

    inline auto async_fallocate(Context& io, UniqueFd& fd, const int mode, const off_t offset,
                                const off_t len) noexcept
    {
        return IoAwaitable{
            io,
            [raw_fd = fd.get(), mode, offset, len](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_fallocate(sqe, raw_fd, mode, static_cast<__u64>(offset),
                                        static_cast<__u64>(len));
            },
            false
        };
    }

    // Needs liburing >= 2.9 for io_uring_prep_ftruncate; this tree pins 2.9.
    inline auto async_ftruncate(Context& io, UniqueFd& fd, const off_t len) noexcept
    {
        return IoAwaitable{
            io,
            [raw_fd = fd.get(), len](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_ftruncate(sqe, raw_fd, len);
            },
            false
        };
    }

    // The CQE carries the returned poll mask bits as a non-negative int.
    inline auto async_poll(Context& io, UniqueFd& fd, const unsigned poll_mask) noexcept
    {
        return IoAwaitable{
            io,
            [raw_fd = fd.get(), poll_mask](io_uring_sqe* sqe, __kernel_timespec*)
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
                const auto sec = std::chrono::duration_cast<std::chrono::seconds>(d);
                ts->tv_sec = sec.count();
                ts->tv_nsec = (d - sec).count();
                io_uring_prep_timeout(sqe, ts, 0, 0);
            },
            /*etime_is_success=*/true
        };
    }

    // ---- Registered (fixed) buffer I/O: lease-first ----
    // buf_index is derived from the lease via the pool's branchless region
    // math; it is never a user parameter. The lease must outlive the
    // co_await (pass it as an lvalue): the SQE borrows ptr/len/index by
    // value while the caller's lease keeps the slot held. Deliberately NO
    // whole-lease convenience overload: a 4th integer argument would be
    // ambiguous between len and off (v1 lesson) -- pass buf.size() for len.
    inline auto async_read_fixed(Context& io, UniqueFd& fd, BufferLease& buf, const size_t len,
                                 const size_t buffer_offset, const std::uint64_t off) noexcept
    {
        CHECK(io.buffer_pool().has_value() && buf.pool() == &*io.buffer_pool())
            << "async_read_fixed: empty or foreign lease (not registered with this Context)";
        CHECK(buffer_offset < buf.size() && len > 0)
            << "async_read_fixed: empty range (buffer_offset past end or zero length)";
        const size_t safe_len = std::min(len, buf.size() - buffer_offset);
        std::byte* ptr = buf.data() + buffer_offset;
        const int buf_index = buf.pool()->registration_index(ptr);
        const int raw_fd = fd.get();
        return IoAwaitable{
            io,
            [raw_fd, ptr, safe_len, off, buf_index](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_read_fixed(sqe, raw_fd, ptr, static_cast<unsigned>(safe_len), off,
                                         buf_index);
            },
            false
        };
    }

    inline auto async_write_fixed(Context& io, UniqueFd& fd, const BufferLease& buf, const size_t len,
                                  const size_t buffer_offset, const std::uint64_t off) noexcept
    {
        CHECK(io.buffer_pool().has_value() && buf.pool() == &*io.buffer_pool())
            << "async_write_fixed: empty or foreign lease (not registered with this Context)";
        CHECK(buffer_offset < buf.size() && len > 0)
            << "async_write_fixed: empty range (buffer_offset past end or zero length)";
        const size_t safe_len = std::min(len, buf.size() - buffer_offset);
        const std::byte* ptr = buf.data() + buffer_offset;
        const int buf_index = buf.pool()->registration_index(ptr);
        const int raw_fd = fd.get();
        return IoAwaitable{
            io,
            [raw_fd, ptr, safe_len, off, buf_index](io_uring_sqe* sqe, __kernel_timespec*)
            {
                io_uring_prep_write_fixed(sqe, raw_fd, ptr, static_cast<unsigned>(safe_len), off,
                                          buf_index);
            },
            false
        };
    }

    inline void cancel(Context& io, IoAwaitableBase& op) noexcept
    {
        io.cancel(op);
    }
} // namespace kio::io
