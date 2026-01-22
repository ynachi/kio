#pragma once

#include <system_error>

#include <liburing.h>

#include "io_context.hpp"

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
class ring_waker
{
public:
    ring_waker()
    {
        int ret = io_uring_queue_init(32, &ring_, 0);
        if (ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "ring_waker");
        }
    }

    ~ring_waker() { io_uring_queue_exit(&ring_); }

    ring_waker(const ring_waker&) = delete;
    ring_waker& operator=(const ring_waker&) = delete;

    void wake(int target_ring_fd) noexcept
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
        unsigned n = io_uring_peek_batch_cqe(&ring_, cqes, 32);
        if (n)
            io_uring_cq_advance(&ring_, n);
    }

    void wake(io_context& ctx) noexcept { wake(ctx.ring_fd()); }

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
class event
{
public:
    explicit event(io_context* ctx) : ctx_(ctx), fd_(eventfd(0, EFD_CLOEXEC))
    {
        if (fd_ < 0)
        {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
    }

    ~event() { ::close(fd_); }

    event(const event&) = delete;
    event& operator=(const event&) = delete;

    struct wait_op : uring_op<wait_op>
    {
        int fd;
        uint64_t value{};

        wait_op(io_context* ctx, int fd) : uring_op(ctx), fd(fd) {}

        void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, &value, sizeof(value), 0); }

        Result<uint64_t> await_resume()
        {
            if (res < 0)
                return std::unexpected(make_error_code(res));
            return value;
        }
    };

    wait_op wait() { return {ctx_, fd_}; }

    void signal(uint64_t count = 1) { [[maybe_unused]] auto r = ::write(fd_, &count, sizeof(count)); }

    int fd() const { return fd_; }

private:
    io_context* ctx_;
    int fd_;
};

}