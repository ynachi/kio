#pragma once
#include <filesystem>
#include <span>
#include <sys/uio.h>

#include <boost/context/detail/fcontext.hpp>

#include "uring/core/fiber.hpp"
#include "uring/core/io.h"

namespace URing
{

// ============================================================================
// FiberIO — synchronous-looking I/O API for stackful fibers
//
// Obtain a FiberIO by spawning a fiber via IO::spawn_fiber(). Every method
// submits one SQE, suspends the calling fiber via swapcontext, and returns
// once the CQE has been processed. From the fiber's perspective the call is
// blocking; the OS thread never blocks — the IO event loop continues running
// other work while this fiber waits.
//
// All methods return Result<T>, consistent with the rest of the codebase.
// Use FIBER_TRY / FIBER_TRY_VOID to propagate errors without boilerplate.
// ============================================================================
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

    [[nodiscard]] Result<int32_t>     write_fixed(Fd& fd, const FixedBuffer& buf, size_t len, off_t offset = -1);
    [[nodiscard]] Result<int32_t>     write_fixed(Fd& fd, const FixedBuffer& buf, off_t offset = -1);
    [[nodiscard]] Result<int32_t>     read_fixed(Fd& fd, FixedBuffer& buf, size_t len, off_t offset = -1);
    [[nodiscard]] Result<int32_t>     read_fixed(Fd& fd, FixedBuffer& buf, off_t offset = -1);
    [[nodiscard]] Result<int32_t>     read(Fd& fd, std::span<std::byte> buf, off_t offset = -1);
    [[nodiscard]] Result<int32_t>     writev(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1);
    [[nodiscard]] Result<void>        fsync(Fd& fd, bool datasync = false);
    [[nodiscard]] Result<Fd>          open(std::filesystem::path path, int flags, mode_t mode = 0644);
    [[nodiscard]] Result<void>        close(Fd&& fd);
    [[nodiscard]] Result<void>        fallocate(Fd& fd, int mode, off_t offset, off_t len);
    [[nodiscard]] Result<void>        ftruncate(Fd& fd, off_t len);
    // Delegates directly — no I/O submission needed
    [[nodiscard]] Result<FixedBuffer> take_fixed_buffer(size_t size);
};

// ============================================================================
// Implementations — defined here because IO is fully known via io.h above.
// ============================================================================

template <typename Setup>
int32_t FiberIO::submit_and_wait(Setup&& setup) noexcept
{
    io_uring_sqe* sqe = io_.get_sqe();
    if (sqe == nullptr) [[unlikely]]
        return -EBUSY;

    std::forward<Setup>(setup)(sqe);
    // Store FiberContext* directly (bit 0 = 1 distinguishes from IoOps* in tick())
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&ctx_) | 1u);
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
        return std::unexpected(make_error_code(res));
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
        return std::unexpected(make_error_code(res));
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
        return std::unexpected(make_error_code(res));
    return res;
}

inline Result<int32_t> FiberIO::writev(Fd& fd, const std::span<const iovec> iovecs, const off_t offset)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
        { io_uring_prep_writev(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); });
    if (res < 0) [[unlikely]]
        return std::unexpected(make_error_code(res));
    return res;
}

inline Result<void> FiberIO::fsync(Fd& fd, const bool datasync)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, datasync](io_uring_sqe* sqe)
        { io_uring_prep_fsync(sqe, raw_fd, datasync ? 0u : IORING_FSYNC_DATASYNC); });
    if (res < 0) [[unlikely]]
        return std::unexpected(make_error_code(res));
    return {};
}

inline Result<Fd> FiberIO::open(std::filesystem::path path, const int flags, const mode_t mode)
{
    // path captured by value so it lives on the fiber stack for the entire kernel op.
    const int32_t res = submit_and_wait(
        [path = std::move(path), flags, mode](io_uring_sqe* sqe)
        { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); });
    if (res < 0) [[unlikely]]
        return std::unexpected(make_error_code(res));
    return Fd{res};
}

inline Result<void> FiberIO::close(Fd&& fd)
{
    const int32_t res = submit_and_wait(
        [fd = std::move(fd)](io_uring_sqe* sqe) mutable
        { io_uring_prep_close(sqe, fd.Release()); });
    if (res < 0) [[unlikely]]
        return std::unexpected(make_error_code(res));
    return {};
}

inline Result<void> FiberIO::fallocate(Fd& fd, const int mode, const off_t offset, const off_t len)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, mode, offset, len](io_uring_sqe* sqe)
        { io_uring_prep_fallocate(sqe, raw_fd, mode, offset, len); });
    if (res < 0) [[unlikely]]
        return std::unexpected(make_error_code(res));
    return {};
}

inline Result<void> FiberIO::ftruncate(Fd& fd, const off_t len)
{
    const int32_t res = submit_and_wait(
        [raw_fd = fd.fd, len](io_uring_sqe* sqe)
        { io_uring_prep_ftruncate(sqe, raw_fd, len); });
    if (res < 0) [[unlikely]]
        return std::unexpected(make_error_code(res));
    return {};
}

inline Result<FixedBuffer> FiberIO::take_fixed_buffer(const size_t size)
{
    return io_.take_fixed_buffer(size);
}

}  // namespace URing
