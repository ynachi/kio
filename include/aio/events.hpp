#pragma once

#include <system_error>

#include <liburing.h>

#include "IoContext.hpp"

namespace aio
{
// -----------------------------------------------------------------------------
// ring_waker - Cross-thread wake via MSG_RING
// -----------------------------------------------------------------------------

/**
 * Sends a wake signal to an io_context on another thread.
 *
 * Usage (e.g., for shutdown):
 *   ring_waker waker;
 *   for (auto& w : workers) {
 *       w.ctx->stop();
 *       waker.wake(*w.ctx);
 *   }
 *
 * Each ring_waker owns a small io_uring used only for MSG_RING.
 * Thread-safe to call wake() from any thread.
 */
class RingWaker
{
public:
    RingWaker()
    {
        if (const int ret = io_uring_queue_init(32, &ring_, 0); ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "ring_waker");
        }
    }

    ~RingWaker() { io_uring_queue_exit(&ring_); }

    RingWaker(const RingWaker&) = delete;
    RingWaker& operator=(const RingWaker&) = delete;

    void Wake(int target_ring_fd) noexcept
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
        {
            (void)io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe)
                return;  // best-effort
        }

        io_uring_prep_msg_ring(sqe, target_ring_fd, /*res*/ 0u, /*user_data*/ WAKE_TAG, 0);
        io_uring_sqe_set_data(sqe, nullptr);
        (void)io_uring_submit(&ring_);

        // Reap CQEs so this tiny ring doesn't fill up.
        io_uring_cqe* cqes[32];
        if (const unsigned n = io_uring_peek_batch_cqe(&ring_, cqes, 32))
            io_uring_cq_advance(&ring_, n);
    }

    void Wake(const IoContext& ctx) noexcept { Wake(ctx.RingFd()); }

private:
    io_uring ring_{};
};

// -----------------------------------------------------------------------------
// Event (eventfd-based signaling)
// -----------------------------------------------------------------------------

/**
 * Simple one-shot or multi-shot event for coroutine signaling.
 *
 * Usage:
 *   event evt{&ctx};
 *
 *   // Waiter coroutine:
 *   auto result = co_await evt.wait().with_timeout(5s);
 *
 *   // Signaler (any thread):
 *   evt.signal();
 */
class Event
{
public:
    explicit Event(IoContext* ctx) : ctx_(ctx), fd_(eventfd(0, EFD_CLOEXEC))
    {
        if (fd_ < 0)
        {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
    }

    ~Event() { ::close(fd_); }

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    struct WaitOp : UringOp<WaitOp>
    {
        int fd;
        size_t value{};

        WaitOp(IoContext* ctx, const int fd) : UringOp(ctx), fd(fd) {}

        void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, &value, sizeof(value), 0); }
    };

    WaitOp Wait() { return {ctx_, fd_}; }

    void Signal(const uint64_t count = 1) const { [[maybe_unused]] auto r = ::write(fd_, &count, sizeof(count)); }

    int Fd() const { return fd_; }

private:
    IoContext* ctx_;
    int fd_;
};

}  // namespace aio