#include "uring/context.h"

#include <cassert>
#include <cstring>

#include <unistd.h>

#include <sys/eventfd.h>

namespace URing
{

IoContext::IoContext(const ContextOptions& opts) : op_pool_(opts.entries), spawn_consumer_token_(spawn_queue_)
{
    io_uring_params params{};

    params.flags |= opts.flags;

    // Safely configure SQPOLL if requested
    if (params.flags & IORING_SETUP_SQPOLL)
    {
        params.sq_thread_idle = opts.sq_thread_idle_ms;
        if (opts.sq_thread_cpu >= 0)
        {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = opts.sq_thread_cpu;
        }
    }

    if (const int rc = io_uring_queue_init_params(opts.entries, &ring_, &params); rc < 0)
    {
        throw std::runtime_error(std::format("io_uring_queue_init_params failed: {}", std::strerror(-rc)));
    }

    ALOG_INFO("Started IO context with {} entries (SQPOLL: {})", opts.entries,
              (opts.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
}

/// Best effort cancellation request
void IoContext::request_cancel(const uint32_t op_idx) noexcept
{
    auto& op = op_pool_.get(op_idx);
    if (!op.cancel())
    {
        return;
    }

    const auto sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr)
    {
        ALOG_WARN("failed to enqueue cancel operation: SQE queue is full");
        return;
    }

    // Kernel matches EXACT original user_data, preventing stale/race cancels
    io_uring_prep_cancel64(sqe, op.original_ud, 0);
    io_uring_sqe_set_data(sqe, nullptr);
}

void IoContext::drain_local()
{
    // We swap the vector so that if a resuming coroutine immediately submits
    // a task that completes synchronously (or adds to the queue), it goes into
    // the NEXT tick's batch, preventing an infinite loop inside this tick.
    process_queue_.swap(ready_queue_);

    for (auto h : process_queue_)
    {
        try
        {
            if (h && !h.done())
            {
                h.resume();
            }
        }
        catch (const std::exception& e)
        {
            ALOG_ERROR("coroutine died with error: {}", e.what());
        }
        catch (...)
        {
            ALOG_ERROR("coroutine died with unknown error");
        }
    }
    process_queue_.clear();
}

void IoContext::tick() noexcept
{
    bool drain_remote_q = false;

    // Skip the blocking syscall if CQEs are already waiting in the ring
    if (io_uring_cq_ready(&ring_) > 0)
    {
        if (const auto ret = io_uring_submit(&ring_); ret < 0 && ret != -EINTR)
        {
            ALOG_ERROR("failed to submit: {}", std::strerror(-ret));
        }
    }
    else
    {
        if (const auto ret = io_uring_submit_and_wait(&ring_, 1); ret < 0 && ret != -EINTR)
        {
            ALOG_ERROR("failed to submit and wait: {}", std::strerror(-ret));
        }
    }

    // Batch process CQEs
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;
    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        count++;
        const auto ud = cqe->user_data;
        if (ud == 0)
        {
            continue;
        }

        if (ud == kWakeTag)
        {
            drain_remote_q = true;

            if (cqe->res < 0)
            {
                ALOG_WARN("wake eventfd read failed: {}", std::strerror(-cqe->res));
            }

            continue;
        }

        const auto token = Token::unpack(ud);
        const auto op = op_pool_.try_get(token);
        if (op == nullptr)
        {
            // Stale CQE from recycled index
            continue;
        }

        op->result_code = cqe->res;
        ready_queue_.push_back(op->handle);
    }

    if (count > 0)
    {
        io_uring_cq_advance(&ring_, count);
    }

    // drain local first, its known as the preferred path
    drain_local();
}

IoContext::~IoContext()
{
    if (ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&ring_);
        ring_.ring_fd = -1;
    }
}
}  // namespace URing
