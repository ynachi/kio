#include "uring/context.h"

#include <cassert>

namespace URing
{
IoContext::IoContext(const std::uint32_t entries, const unsigned flags)
{
    io_uring_params params{};

    params.flags |= flags;
    io_uring_queue_init_params(entries, &m_ring_, &params);

    // Initialize the intrusive free list
    m_op_slab_.resize(entries);
    for (std::uint32_t i = 0; i < entries; ++i)
    {
        m_op_slab_[i].next_free_idx = i + 1u;
    }
}

// TODO: allow ops slab resize or return an error
Token IoContext::allocate_token() noexcept
{
    assert(m_head_free_idx_ < m_op_slab_.size() && "IoContext ran out of operation slots");
    const uint32_t idx = m_head_free_idx_;
    m_head_free_idx_ = m_op_slab_[idx].next_free_idx;
    m_op_slab_[idx].gen++;
    ++m_pending_ops_;
    return {idx, m_op_slab_[idx].gen};
}

void IoContext::free_token(Token t) noexcept
{
    assert(t.idx < m_op_slab_.size() && "Attempted to free an invalid token");
    assert(m_pending_ops_ > 0 && "Attempted to free a token when no operations are pending");
    m_op_slab_[t.idx].next_free_idx = m_head_free_idx_;
    m_head_free_idx_ = t.idx;
    --m_pending_ops_;
}

void IoContext::tick(std::chrono::nanoseconds timeout_ns) noexcept
{
    // drain the runnable queue first
    const auto runnable = std::move(m_runnable_queue);
    m_runnable_queue.clear();
    for (const auto& coro : runnable)
    {
        coro.resume();
    }

    // 2. If we have no pending I/O, just flush and return immediately.
    //    The run() loop will check the stop_token and exit if requested.
    if (m_pending_ops_ == 0)
    {
        io_uring_submit(&m_ring_);
        return;
    }

    // Convert std::chrono::nanoseconds to __kernel_timespec safely
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout_ns);
    auto nsecs = timeout_ns - secs;  // The remaining nanoseconds

    __kernel_timespec ts{.tv_sec = static_cast<long long>(secs.count()),
                         .tv_nsec = static_cast<long long>(nsecs.count())};

    io_uring_submit_and_wait_timeout(&m_ring_, nullptr, 1, &ts, nullptr);

    // Batch process CQEs
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;
    io_uring_for_each_cqe(&m_ring_, head, cqe)
    {
        if (cqe->user_data == 0)
        {
            ++count;
            continue;
        }  // cancel sentinel
        on_cqe(cqe);
        ++count;
    }

    if (count > 0)
    {
        io_uring_cq_advance(&m_ring_, count);
    }
}

void IoContext::on_cqe(const io_uring_cqe* cqe) noexcept
{
    const Token t = Token::from_u64(cqe->user_data);
    OpState& state = m_op_slab_[t.idx];

    if (state.gen != t.gen)
    {
        return;
    }

    if (state.is_abandoned)
    {
        free_token(t);
        return;
    }

    state.cqe_res = cqe->res;
    m_runnable_queue.push_back(state.coro_handle);
}
}  // namespace URing
