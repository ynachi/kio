#pragma once
#include <cstdint>
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

    void tick() noexcept;

public:
    explicit IoContext(std::uint32_t entries = 16800, unsigned flags = kUringDefaultFlag);

    Token allocate_token() noexcept;

    void free_token(Token t) noexcept;

    OpState& get_state(const Token t) noexcept { return m_op_slab_[t.idx]; }
    io_uring_sqe* get_sqe() noexcept { return io_uring_get_sqe(&m_ring_); }

    void submit_cancel(const Token t) noexcept
    {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel64(sqe, t.to_u64(), 0);
        io_uring_sqe_set_data64(sqe, 0);
    }

    void on_cqe(const io_uring_cqe* cqe) noexcept;

    void run(std::stop_token st) noexcept;

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
