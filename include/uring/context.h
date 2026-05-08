#pragma once
#include <cassert>
#include <chrono>
#include <stop_token>
#include <vector>

#include <liburing.h>

#include "token.hpp"

namespace URing
{
constexpr unsigned kUringDefaultFlag =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

class IoContext
{
    io_uring m_ring_{};
    std::vector<OpState> m_op_slab_;
    uint32_t m_head_free_idx_ = 0;
    uint32_t m_pending_ops_ = 0;

    // Local queue for immediate resumptions to avoid deep call stacks
    std::vector<std::coroutine_handle<>> m_runnable_queue;

    void tick(std::chrono::nanoseconds timeout_ns) noexcept;

public:
    explicit IoContext(std::uint32_t entries = 16800, unsigned flags = kUringDefaultFlag);

    Token allocate_token() noexcept;

    void free_token(Token t) noexcept;

    OpState& get_state(const Token t) noexcept { return m_op_slab_[t.idx]; }
    io_uring_sqe* get_sqe() noexcept { return io_uring_get_sqe(&m_ring_); }
    io_uring_sqe* get_sqe_safe() noexcept
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring_);
        if (!sqe)
        {
            // The SQ ring is full. Flush the current batch to the kernel.
            io_uring_submit(&m_ring_);
            sqe = io_uring_get_sqe(&m_ring_);
            assert(sqe != nullptr && "SQ ring still full after flush!");
        }
        return sqe;
    }

    void submit_cancel(const Token t) noexcept
    {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel64(sqe, t.to_u64(), 0);
        io_uring_sqe_set_data64(sqe, 0);
    }

    void on_cqe(const io_uring_cqe* cqe) noexcept;

    // Templated run accepts any chrono duration, defaults to 10ms
    /// tick_timeout is the time a tick will wait for the kernel IO to arrive
    /// And alternative would have been an eventfd but lets start here for now
    template <typename Rep, typename Period>
    void run(std::stop_token st, std::chrono::duration<Rep, Period> tick_timeout) noexcept
    {
        auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_timeout);
        while (!st.stop_requested() && (m_pending_ops_ != 0 || !m_runnable_queue.empty()))
        {
            tick(timeout_ns);
        }
    }

    // Convenience overload for the default timeout
    void run(std::stop_token st) noexcept { run(std::move(st), std::chrono::milliseconds(10)); }

    ~IoContext()
    {
        if (m_ring_.ring_fd > 0)
        {
            io_uring_queue_exit(&m_ring_);
            m_ring_.ring_fd = -1;
        }
    }
};
}  // namespace URing
