#pragma once
#include <filesystem>
#include <span>
#include <sys/uio.h>

#include <boost/context/detail/fcontext.hpp>

#include "uring/core/fiber.hpp"
#include "uring/core/io.h"

namespace URing
{

/// Synchronous-looking I/O API for stackful fibers.
///
/// Obtain a FiberIO by spawning a fiber via IO::spawn_fiber() or
/// IO::schedule_fiber().  Every I/O method submits one SQE, suspends the
/// calling fiber via jump_fcontext, and returns once the CQE has arrived.
/// From the fiber's perspective the call is blocking; the OS thread never
/// blocks — the event loop continues running other coroutines and fibers
/// while this one waits.
///
/// All methods return Result<T> (@ref URing::Result).  Use FIBER_TRY and
/// FIBER_TRY_VOID to propagate errors without boilerplate:
///
/// @code
/// Result<void> handler(FiberIO& fio)
/// {
///     FIBER_TRY(auto fd, fio.open("/var/data/log", O_RDONLY));
///     FIBER_TRY(auto buf, fio.take_fixed_buffer(4096));
///     FIBER_TRY(auto n,   fio.read_fixed(fd, buf));
///     // buf.ptr()[0..n-1] now holds the data
///     FIBER_TRY_VOID(fio.close(std::move(fd)));
///     return {};
/// }
/// @endcode
class FiberIO
{
    IO&           io_;
    FiberContext& ctx_;

    // Submit one SQE tagged as a fiber op and suspend until the CQE arrives.
    // setup(sqe) is called before the suspension; its captures live on the
    // fiber stack and remain valid until the fiber resumes.
    template <typename Setup>
    int32_t submit_and_wait(Setup&& setup) noexcept;

public:
    FiberIO(IO& io, FiberContext& ctx) noexcept : io_(io), ctx_(ctx) {}

    // ── Sync-primitive support ────────────────────────────────────────────
    // Building blocks for FiberMutex, FiberSemaphore, and FiberChannel.
    // Prefer those types over calling suspend/wakeup directly.

    /// Suspend this fiber until a sync primitive calls wakeup() on it.
    ///
    /// The OS thread is not blocked — the event loop continues running other
    /// fibers and coroutines.  Returns only after another fiber (or the same
    /// fiber after it completes a wakeup) re-enqueues this context.
    ///
    /// Prefer the higher-level primitives in fiber_sync.hpp.  Only call this
    /// directly when building a new synchronization abstraction.
    Result<void> suspend() noexcept
    {
        ctx_.state = FiberState::SyncWait;
        auto t = boost::context::detail::jump_fcontext(ctx_.scheduler_ctx, nullptr);
        ctx_.scheduler_ctx = t.fctx;
        if (ctx_.last_res < 0) [[unlikely]]
            return std::unexpected(make_error_code(ctx_.last_res));
        return {};
    }

    /// Re-enqueue @p ctx so the event loop resumes it on its next tick.
    ///
    /// @p ctx must be a fiber that previously called suspend() and has not yet
    /// been woken.  Calling wakeup() on an already-running or already-woken
    /// fiber is undefined behaviour.
    ///
    /// Prefer the higher-level primitives in fiber_sync.hpp.
    void wakeup(FiberContext& ctx) noexcept
    {
        assert(ctx.io == &io_ &&
               "FiberMutex/Semaphore/Channel must not be shared across IO workers");
        ctx.last_res = 0;
        ctx.state = FiberState::Ready;
        io_.ready_fibers_.push_back(&ctx);
    }

    /// Return this fiber's execution context.
    ///
    /// Sync primitives store the returned reference in their waiter queues and
    /// pass it to wakeup() when the resource becomes available.  Do not store
    /// this reference beyond the fiber's lifetime.
    [[nodiscard]] FiberContext& context() noexcept { return ctx_; }

    // ── I/O operations ───────────────────────────────────────────────────

    /// Write up to @p len bytes from @p buf (a registered fixed buffer) to @p fd
    /// starting at @p offset.
    ///
    /// Uses io_uring's WRITE_FIXED operation, which avoids the kernel copy into
    /// a pinned buffer because @p buf is already registered with the ring.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Number of bytes written, or an error code.
    ///
    /// @code
    /// FIBER_TRY(auto buf, fio.take_fixed_buffer(4096));
    /// std::memcpy(buf.ptr(), data.data(), data.size());
    /// FIBER_TRY(auto n, fio.write_fixed(fd, buf, data.size(), file_offset));
    /// @endcode
    [[nodiscard]] Result<int32_t>     write_fixed(Fd& fd, const FixedBuffer& buf, size_t len, off_t offset = -1);

    /// Write the whole contents of @p buf to @p fd starting at @p offset.
    /// Equivalent to write_fixed(fd, buf, buf.size(), offset).
    [[nodiscard]] Result<int32_t>     write_fixed(Fd& fd, const FixedBuffer& buf, off_t offset = -1);

    /// Read up to @p len bytes from @p fd into @p buf (a registered fixed buffer)
    /// starting at @p offset.
    ///
    /// Uses io_uring's READ_FIXED operation.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Number of bytes read, or an error code.
    ///
    /// @code
    /// FIBER_TRY(auto buf, fio.take_fixed_buffer(4096));
    /// FIBER_TRY(auto n, fio.read_fixed(fd, buf, 4096, 0));
    /// std::string_view content{reinterpret_cast<const char*>(buf.ptr()),
    ///                          static_cast<size_t>(n)};
    /// @endcode
    [[nodiscard]] Result<int32_t>     read_fixed(Fd& fd, FixedBuffer& buf, size_t len, off_t offset = -1);

    /// Read up to buf.size() bytes from @p fd into @p buf starting at @p offset.
    /// Equivalent to read_fixed(fd, buf, buf.size(), offset).
    [[nodiscard]] Result<int32_t>     read_fixed(Fd& fd, FixedBuffer& buf, off_t offset = -1);

    /// Read up to buf.size() bytes from @p fd into the caller-provided span @p buf.
    ///
    /// Unlike read_fixed, this works with any user buffer but requires the kernel
    /// to copy data through an intermediate bounce buffer.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Number of bytes read, or an error code.
    [[nodiscard]] Result<int32_t>     read(Fd& fd, std::span<std::byte> buf, off_t offset = -1);

    /// Write the scatter-gather vector @p iovecs to @p fd starting at @p offset.
    ///
    /// Wraps pwritev2 via io_uring's WRITEV operation.
    /// Pass @p offset = -1 to use the file's current position.
    ///
    /// @return Total bytes written, or an error code.
    [[nodiscard]] Result<int32_t>     writev(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1);

    /// Flush pending writes on @p fd to the storage device.
    ///
    /// @param datasync  If true, flushes data only (fdatasync semantics —
    ///                  metadata such as mtime may lag).  If false, flushes
    ///                  both data and metadata (fsync semantics).
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void>        fsync(Fd& fd, bool datasync = false);

    /// Open or create the file at @p path and return a file descriptor.
    ///
    /// @param path
    /// @param flags  Standard open(2) flags such as O_RDONLY, O_CREAT | O_WRONLY.
    /// @param mode   Permission bits used when O_CREAT creates a new file.
    ///               Ignored if O_CREAT is not set.
    /// @return An owned Fd on success, or an error code (e.g. ENOENT, EACCES).
    ///
    /// @code
    /// FIBER_TRY(auto fd, fio.open("/var/log/app.log",
    ///                              O_CREAT | O_WRONLY | O_APPEND, 0644));
    /// @endcode
    [[nodiscard]] Result<Fd>          open(std::filesystem::path path, int flags, mode_t mode = 0644);

    /// Close @p fd asynchronously via io_uring.
    ///
    /// Takes ownership of @p fd so that it is released exactly once even if the
    /// close CQE returns an error.  Callers should use std::move:
    ///
    /// @code
    /// FIBER_TRY_VOID(fio.close(std::move(fd)));
    /// @endcode
    [[nodiscard]] Result<void>        close(Fd&& fd);

    /// Allocate or deallocate disk space for @p fd in the range [@p offset, @p offset + @p len).
    ///
    /// @param mode  fallocate(2) mode flags (0 for a simple pre-allocation,
    ///              FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE to punch a hole, etc.).
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void>        fallocate(Fd& fd, int mode, off_t offset, off_t len);

    /// Truncate or extend @p fd to exactly @p len bytes.
    ///
    /// Requires kernel 6.9+ and liburing 2.6+.
    /// @return {} on success, or an error code.
    [[nodiscard]] Result<void>        ftruncate(Fd& fd, off_t len);

    /// Borrow a fixed buffer of at least @p size bytes from the IO's pool.
    ///
    /// Fixed buffers are pre-registered with io_uring and enable zero-copy I/O
    /// via read_fixed / write_fixed.  The buffer is returned to the pool when
    /// the returned FixedBuffer object is destroyed.
    ///
    /// The IO must have been constructed with a non-empty pool_configs list.
    ///
    /// @return A FixedBuffer handle, or an error if no buffer of the requested
    ///         size is available.
    ///
    /// @code
    /// FIBER_TRY(auto buf, fio.take_fixed_buffer(4096));
    /// FIBER_TRY(auto n,   fio.read_fixed(fd, buf));
    /// @endcode
    [[nodiscard]] Result<FixedBuffer> take_fixed_buffer(size_t size);
};

// ============================================================================
// Implementations — defined here because IO is fully known via io.h above.
// ============================================================================

template <typename Setup>
int32_t FiberIO::submit_and_wait(Setup&& setup) noexcept
{
    if (io_.canceling_fibers_) [[unlikely]]
    {
        return -ECANCELED;
    }

    io_uring_sqe* sqe = io_.get_sqe();
    if (sqe == nullptr) [[unlikely]]
    {
        return -EBUSY;
    }

    std::forward<Setup>(setup)(sqe);
    // Store FiberContext* directly (bit 0 = 1 distinguishes from IoOps* in tick())
    ctx_.pending_user_data = reinterpret_cast<uint64_t>(&ctx_) | 1u;
    ctx_.state = FiberState::IoWait;
    io_uring_sqe_set_data64(sqe, ctx_.pending_user_data);
    // Suspend fiber: jump back to tick().  t.fctx is tick's saved context
    // (updated each resume so it always points to the current tick call site).
    auto t = boost::context::detail::jump_fcontext(ctx_.scheduler_ctx, nullptr);
    ctx_.scheduler_ctx = t.fctx;
    // tick() wrote ctx_.last_res from the CQE before jumping here
    return ctx_.last_res;
}

inline Result<int32_t> FiberIO::write_fixed(Fd& fd, const FixedBuffer& buf, const size_t len, const off_t offset)
{
    const size_t safe_len = std::min(len, buf.size());
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, ptr = buf.ptr(), safe_len, idx = buf.index(), offset](io_uring_sqe* sqe)
        { io_uring_prep_write_fixed(sqe, raw_fd, ptr, safe_len, offset, idx); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return res;
}

inline Result<int32_t> FiberIO::write_fixed(Fd& fd, const FixedBuffer& buf, const off_t offset)
{
    return write_fixed(fd, buf, buf.size(), offset);
}

inline Result<int32_t> FiberIO::read_fixed(Fd& fd, FixedBuffer& buf, const size_t len, const off_t offset)
{
    const size_t safe_len = std::min(len, buf.size());
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, ptr = buf.ptr(), safe_len, idx = buf.index(), offset](io_uring_sqe* sqe)
        { io_uring_prep_read_fixed(sqe, raw_fd, ptr, safe_len, offset, idx); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return res;
}

inline Result<int32_t> FiberIO::read_fixed(Fd& fd, FixedBuffer& buf, const off_t offset)
{
    return read_fixed(fd, buf, buf.size(), offset);
}

inline Result<int32_t> FiberIO::read(Fd& fd, const std::span<std::byte> buf, const off_t offset)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
        { io_uring_prep_read(sqe, raw_fd, buf.data(), buf.size(), offset); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return res;
}

inline Result<int32_t> FiberIO::writev(Fd& fd, const std::span<const iovec> iovecs, const off_t offset)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
        { io_uring_prep_writev(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return res;
}

inline Result<void> FiberIO::fsync(Fd& fd, const bool datasync)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, datasync](io_uring_sqe* sqe)
        { io_uring_prep_fsync(sqe, raw_fd, datasync ? IORING_FSYNC_DATASYNC : 0u); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return {};
}

inline Result<Fd> FiberIO::open(std::filesystem::path path, const int flags, const mode_t mode)
{
    // path captured by value so it lives on the fiber stack for the entire kernel op.
    const int32_t res = submit_and_wait(
        [path = std::move(path), flags, mode](io_uring_sqe* sqe)
        { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return Fd{res};
}

inline Result<void> FiberIO::close(Fd&& fd)
{
    const int32_t res = submit_and_wait(
        [fd = std::move(fd)](io_uring_sqe* sqe) mutable
        { io_uring_prep_close(sqe, fd.Release()); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return {};
}

inline Result<void> FiberIO::fallocate(Fd& fd, const int mode, const off_t offset, const off_t len)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, mode, offset, len](io_uring_sqe* sqe)
        { io_uring_prep_fallocate(sqe, raw_fd, mode, offset, len); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return {};
}

inline Result<void> FiberIO::ftruncate(Fd& fd, const off_t len)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, len](io_uring_sqe* sqe)
        { io_uring_prep_ftruncate(sqe, raw_fd, len); });
    if (res < 0) [[unlikely]]
    {
        return std::unexpected(make_error_code(res));
    }
    return {};
}

inline Result<FixedBuffer> FiberIO::take_fixed_buffer(const size_t size)
{
    return io_.take_fixed_buffer(size);
}

}  // namespace URing
