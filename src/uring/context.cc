#include "uring/context.h"

namespace URing
{
IoContext::IoContext(const std::uint32_t entries, const unsigned flags)
{
    io_uring_params params{};

    params.flags |= flags;
    io_uring_queue_init_params(entries, &m_ring_, &params);

    // Initialize the intrusive free list
    m_op_slab_.reserve(entries);
    for (std::uint32_t i = 0; i < entries; ++i)
    {
        m_op_slab_[i].next_free_idx = i + 1;
    }
}

Token IoContext::allocate_token() noexcept
{
    const uint32_t idx = m_head_free_idx_;
    m_head_free_idx_ = m_op_slab_[idx].next_free_idx;
    m_op_slab_[idx].gen++;
    return {idx, m_op_slab_[idx].gen};
}

void IoContext::free_token(Token t) noexcept
{
    m_op_slab_[t.idx].next_free_idx = m_head_free_idx_;
    m_head_free_idx_ = t.idx;
}

void IoContext::tick() noexcept
{
    // drain the runnable queue first
    for (const auto& coro : m_runnable_queue)
    {
        coro.resume();
    }

    // Submit pending work and wait (DEFER_TASKRUN makes this highly efficient)
    io_uring_submit_and_wait(&m_ring_, 1);

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

    // TODO: if count == 0 is noop but lets check if we have to guard against it
    io_uring_cq_advance(&m_ring_, count);
}

void IoContext::on_cqe(const io_uring_cqe* cqe) noexcept
{
    const Token t = Token::from_u64(cqe->user_data);
    OpState& state = m_op_slab_[t.idx];

    if (state.gen != t.gen || state.is_abandoned)
    {
        free_token(t);
    }

    state.cqe_res = cqe->res;
    m_runnable_queue.push_back(state.coro_handle);
}

void IoContext::run(std::stop_token st) noexcept
{
    while (!st.stop_requested() && !m_runnable_queue.empty())
    {
        tick();
    }
}
}  // namespace URing