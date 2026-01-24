#pragma once

#include <cstdint>
#include <system_error>

#include <unistd.h>

#include <sys/eventfd.h>

#include "io_context.hpp"

namespace aio
{
// -----------------------------------------------------------------------------
// RingWaker - Cross-thread wake via eventfd
// -----------------------------------------------------------------------------

/**
 * Sends a wake signal to an io_context on another thread.
 *
 * Usage (e.g., for shutdown):
 * RingWaker waker;
 * for (auto& w : workers) {
 * w.ctx->stop();
 * waker.Wake(w.ctx->WakeFd());
 * }
 *
 * Lightweight wrapper around write(). Thread-safe.
 */
class RingWaker
{
public:
    RingWaker() = default;
    ~RingWaker() = default;

    /// Wake returns true if succeeded and false otherwise
    static bool Wake(const int wake_fd) noexcept
    {
        uint64_t val = 1;
        // Writing to the eventfd held by IoContext interrupts io_uring_wait_cqe
        // because IoContext keeps a persistent read on it.
        return ::write(wake_fd, &val, sizeof(val)) == sizeof(val);
    }

    static void Wake(const IoContext& ctx) noexcept { Wake(ctx.WakeFd()); }
};
}  // namespace aio