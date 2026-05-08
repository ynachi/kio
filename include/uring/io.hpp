#pragma once
#include "context.h"

#include <span>

#include <liburing.h>

#include "awaiter.hpp"
#include "fd.hpp"

namespace URing
{
inline auto pread(IoContext& ctx, Fd& fd, std::span<std::byte> buf, off_t offset)
{
    return IoAwaiter(ctx, [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
                     { io_uring_prep_read(sqe, raw_fd, buf.data(), buf.size(), offset); });
}

inline auto read(IoContext& ctx, Fd& fd, std::span<std::byte> buf)
{
    return pread(ctx, fd, buf, 0);
}

inline auto pwrite(IoContext& ctx, Fd& fd, std::span<const std::byte> buf, off_t offset)
{
    return IoAwaiter(ctx, [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
                     { io_uring_prep_write(sqe, raw_fd, buf.data(), buf.size(), offset); });
}

inline auto write(IoContext& ctx, Fd& fd, std::span<const std::byte> buf)
{
    return pwrite(ctx, fd, buf, 0);
}
}  // namespace URing