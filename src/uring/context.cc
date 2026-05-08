#include "uring/context.h"

#include <cassert>
#include <cerrno>
#include <utility>

#include <unistd.h>

#include <sys/eventfd.h>

namespace URing
{
IoContext::IoContext(const std::uint32_t entries, const unsigned flags) : m_owner_thread_(std::this_thread::get_id())
{
    io_uring_params params{};

    params.flags |= flags;
    const int rc = io_uring_queue_init_params(entries, &m_ring_, &params);
    assert(rc == 0 && "io_uring_queue_init_params failed");
    if (rc != 0)
    {
        throw std::runtime_error("io_uring_queue_init_params failed");
    }

    // Initialize the intrusive free list
    m_op_slab_.resize(entries);
    for (std::uint32_t i = 0; i < entries; ++i)
    {
        m_op_slab_[i].next_free_idx = i + 1u;
    }

    m_eventfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_eventfd < 0)
    {
        throw std::runtime_error("eventfd failed");
    }

    arm_eventfd();
}

void IoContext::arm_eventfd() noexcept
{
    io_uring_sqe* sqe = get_sqe_safe();
    io_uring_prep_read(sqe, m_eventfd, &m_eventfd_buf, sizeof(m_eventfd_buf), 0);
    io_uring_sqe_set_data64(sqe, kWakeUserData);
}

void IoContext::write_eventfd() const
{
    constexpr uint64_t wake = 1;
    // TODO: add max retry
    while (true)
    {
        if (const auto written = ::write(m_eventfd, &wake, sizeof(wake)); written == sizeof(wake) || errno == EAGAIN)
        {
            return;
        }
        if (errno != EINTR)
        {
            assert(false && "eventfd wake failed");
            return;
        }
    }
}

void IoContext::process_foreign_jobs() noexcept
{
    // 1. FAIRNESS: Process a maximum of 64 jobs per wakeup
    Job jobs[kMaxJobsPerWakeup];

    const size_t dequeued_count = m_foreign_queue_.try_dequeue_bulk(jobs, kMaxJobsPerWakeup);

    for (size_t i = 0; i < dequeued_count; ++i)
    {
        try
        {
            jobs[i]();
        }
        catch (...)
        {
            // std::cerr << "Foreign job threw unknown exception\n";
        }

        // CRITICAL MEMORY MANAGEMENT:
        // Clear the job after executing it. If the lambda captured large
        // objects (like shared_ptrs or buffers), we want to release them
        // immediately, rather than waiting for this array slot to be
        // overwritten on the next wakeup.
        jobs[i] = nullptr;
    }

    // Re-arm the eventfd to catch the next cross-thread ping
    arm_eventfd();

    // Self-ping if we hit the limit, meaning there are likely more jobs waiting
    if (dequeued_count == kMaxJobsPerWakeup)
    {
        write_eventfd();
    }
}

void IoContext::submit_job(Job job) noexcept
{
    m_foreign_queue_.enqueue(std::move(job));

    write_eventfd();
}

void IoContext::assert_owner() noexcept
{
    assert(m_owner_thread_ == std::this_thread::get_id() && "IoContext used from a non-owner thread");
}

// TODO: allow ops slab resize or return an error
Token IoContext::allocate_token() noexcept
{
    assert_owner();
    assert(m_head_free_idx_ < m_op_slab_.size() && "IoContext ran out of operation slots");
    const uint32_t idx = m_head_free_idx_;
    m_head_free_idx_ = m_op_slab_[idx].next_free_idx;
    m_op_slab_[idx].gen++;
    ++m_pending_ops_;
    return {idx, m_op_slab_[idx].gen};
}

void IoContext::free_token(Token t) noexcept
{
    assert_owner();
    assert(t.idx < m_op_slab_.size() && "Attempted to free an invalid token");
    assert(m_pending_ops_ > 0 && "Attempted to free a token when no operations are pending");
    m_op_slab_[t.idx].next_free_idx = m_head_free_idx_;
    m_head_free_idx_ = t.idx;
    --m_pending_ops_;
}

void IoContext::tick(std::chrono::nanoseconds timeout_ns) noexcept
{
    assert_owner();

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
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout_ns);
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
    assert_owner();
    // work on external jobs first
    process_foreign_jobs();
    
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

IoContext::~IoContext()
{
    if (m_ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&m_ring_);
        m_ring_.ring_fd = -1;
    }

    if (m_eventfd >= 0)
    {
        ::close(m_eventfd);
        m_eventfd = -1;
    }
}
}  // namespace URing
