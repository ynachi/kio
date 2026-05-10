#include "uring/context.h"

#include <cassert>

#include <unistd.h>

#include <sys/eventfd.h>

#include "uring/tracer.hpp"

namespace URing
{
IoContext::IoContext(const std::uint32_t entries, const unsigned flags)
    : op_pool_(entries), spawn_consumer_token_(spawn_queue_), owner_thread_(std::this_thread::get_id())
{
    io_uring_params params{};

    params.flags |= flags;
    if (const int rc = io_uring_queue_init_params(entries, &ring_, &params); rc < 0)
    {
        throw std::runtime_error("io_uring_queue_init_params failed");
    }

    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0)
    {
        throw std::runtime_error("eventfd failed");
    }
    arm_wake_read();
}

void IoContext::arm_wake_read() noexcept
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            // Serious backpressure case. Try again from the next tick.
            wake_read_armed_ = false;
            return;
        }
    }

    wake_read_armed_ = true;

    io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);

    io_uring_sqe_set_data64(sqe, kWakeTag);
}

void IoContext::drain_remote_spawns()
{
    std::array<std::move_only_function<DetachedTask(IoContext&)>, kMaxRemoteDrainPerTick> funcs;

    const std::size_t n = spawn_queue_.try_dequeue_bulk(spawn_consumer_token_, funcs.begin(), kMaxRemoteDrainPerTick);

    for (std::size_t i = 0; i < n; ++i)
    {
        funcs[i](*this);
    }
}

void IoContext::request_cancel(const uint32_t op_idx) noexcept
{
    auto& op = op_pool_.get(op_idx);
    if (!op.cancel())
    {
        return;
    }

    Tracer::cancel(Token{op_idx, op.generation});

    const auto sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        return;  // Best-effort cancel if SQ is full
    }

    // Kernel matches EXACT original user_data, preventing stale/race cancels
    io_uring_prep_cancel64(sqe, op.original_ud, 0);
    io_uring_sqe_set_data(sqe, nullptr);  // Cancel CQE ignored by reactor
}

void IoContext::tick(const std::chrono::nanoseconds timeout_ns) noexcept
{
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout_ns);
    auto nsecs = timeout_ns - secs;

    __kernel_timespec ts{.tv_sec = static_cast<long long>(secs.count()),
                         .tv_nsec = static_cast<long long>(nsecs.count())};

    io_uring_cqe* wait_cqe = nullptr;

    io_uring_submit_and_wait_timeout(&ring_, &wait_cqe, 1, &ts, nullptr);

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
            continue;  // cancel, ignore
        }

        if (ud == kWakeTag)
        {
            wake_read_armed_ = false;

            if (cqe->res == sizeof(uint64_t))
            {
                drain_remote_spawns();
            }

            arm_wake_read();
            continue;
        }

        const auto token = Token::unpack(ud);
        const auto op = op_pool_.try_get(token);
        if (op == nullptr)
        {
            continue;  // Stale CQE from recycled index
        }

        Tracer::complete(token, cqe->res);

        op->result_code = cqe->res;
        ready_queue_.push_back(op->handle);
    }

    if (count > 0)
    {
        io_uring_cq_advance(&ring_, count);
    }

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
                Tracer::wake();
                h.resume();
            }
            // Note: We DO NOT call h.destroy() here anymore.
            // Remember the `safe_destroy` logic we put in ~Task() and final_awaiter.
            // The Task memory lifecycle is fully self-managing now.
        }
        catch (...)
        {
            // Log catastrophic user exception
        }
    }
    process_queue_.clear();
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
