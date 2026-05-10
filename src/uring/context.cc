#include "uring/context.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>

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

    wake_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wake_fd_ < 0)
    {
        io_uring_queue_exit(&ring_);
        throw std::runtime_error("eventfd failed");
    }
    arm_wake_read();
    ALOG_INFO("Started IO context with {} entries", entries);
}

void IoContext::wake() const noexcept
{
    constexpr uint64_t one = 1;
    const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
    if (n != sizeof(one) && errno != EINTR)
    {
        ALOG_WARN("failed to wake io context with eventfd: {}", std::strerror(errno));
    }
}

void IoContext::arm_wake_read() noexcept
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        if (const auto ret = io_uring_submit(&ring_); ret < 0)
        {
            ALOG_ERROR("io_uring_submit failed with err {}", std::strerror(-ret));
        }
        sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            ALOG_DEBUG("failed to get a sqe, ring queue is full");
            // Serious backpressure case. Try again from the next tick.
            wake_read_armed_ = false;
            return;
        }
    }

    wake_read_armed_ = true;

    io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);

    io_uring_sqe_set_data64(sqe, kWakeTag);
}

void IoContext::drain_remote_spawns() noexcept
{
    std::array<std::move_only_function<void(IoContext&)>, kMaxRemoteDrainPerTick> funcs;

    const std::size_t n = spawn_queue_.try_dequeue_bulk(spawn_consumer_token_, funcs.begin(), funcs.size());

    for (std::size_t i = 0; i < n; ++i)
    {
        try
        {
            funcs[i](*this);
        }
        catch (const std::exception& e)
        {
            ALOG_ERROR("detached task died with error: {}", e.what());
        }
        catch (...)
        {
            ALOG_ERROR("detached task died with error");
            // Fire-and-forget spawn failures cannot be reported to the caller.
        }
    }

    if (n == funcs.size())
    {
        wake();
    }
}

void IoContext::request_cancel(const uint32_t op_idx) noexcept
{
    auto& op = op_pool_.get(op_idx);
    if (!op.cancel())
    {
        return;
    }

#if URING_ENABLE_TRACING
    Tracer::cancel(Token{op_idx, op.generation});
#endif

    const auto sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr)
    {
        ALOG_WARN("failed to enqueue cancel operation: SQE queue is full");
        return;
    }

    // Kernel matches EXACT original user_data, preventing stale/race cancels
    io_uring_prep_cancel64(sqe, op.original_ud, 0);
    io_uring_sqe_set_data(sqe, nullptr);  // Cancel CQE ignored by reactor
}

void IoContext::tick() noexcept
{
    if (!wake_read_armed_)
    {
        arm_wake_read();
    }

    const auto ret = io_uring_submit_and_wait(&ring_, 1);
    if (ret < 0 && ret != -EINTR)
    {
        ALOG_ERROR("failed to submit and wait: {}", std::strerror(-ret));
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
            continue;  // cancel, ignore
        }

        if (ud == kWakeTag)
        {
            wake_read_armed_ = false;

            if (cqe->res == sizeof(uint64_t))
            {
                drain_remote_spawns();
            }
            else if (cqe->res < 0)
            {
                ALOG_WARN("wake eventfd read failed: {}", std::strerror(-cqe->res));
            }
            else
            {
                ALOG_WARN("wake eventfd read returned unexpected size {}", cqe->res);
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

#if URING_ENABLE_TRACING
        Tracer::complete(token, cqe->res, op->op_name);
#endif

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
#if URING_ENABLE_TRACING
                Tracer::wake();
#endif
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

IoContext::~IoContext()
{
    if (ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&ring_);
        ring_.ring_fd = -1;
    }

    if (wake_fd_ >= 0)
    {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}
}  // namespace URing
