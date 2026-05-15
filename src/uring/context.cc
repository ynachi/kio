#include "uring/context.h"

#include <cassert>
#include <cstring>
#include <future>

#include <unistd.h>

#include <sys/eventfd.h>

namespace URing
{

void IoWorker::init(InternalKey, const int wq_fd)
{
    io_uring_params params{};

    params.flags |= opts_.flags;

    // attach WQ
    if (wq_fd >= 0)
    {
        params.flags |= IORING_SETUP_ATTACH_WQ;
        params.wq_fd = wq_fd;
    }

    // Safely configure SQPOLL if requested
    if (params.flags & IORING_SETUP_SQPOLL)
    {
        params.sq_thread_idle = opts_.sq_thread_idle_ms;
        if (opts_.sq_thread_cpu >= 0)
        {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = opts_.sq_thread_cpu;
        }
    }

    if (const int rc = io_uring_queue_init_params(opts_.entries, &ring_, &params); rc < 0)
    {
        throw std::runtime_error(std::format("io_uring_queue_init_params failed: {}", std::strerror(-rc)));
    }

    // set the current io
    tl_io = this;

    ALOG_INFO("Started IO context with {} entries (SQPOLL: {})", opts_.entries,
              (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
}

/// Best effort cancellation request
void IoWorker::request_cancel(InternalKey key, const uint32_t op_idx) noexcept
{
    auto& op = op_pool_.get(op_idx);
    if (!op.cancel())
    {
        return;
    }

    const auto sqe = get_sqe(key);
    if (sqe == nullptr)
    {
        ALOG_WARN("failed to enqueue cancel operation: SQE queue is full");
        return;
    }

    // Kernel matches EXACT original user_data, preventing stale/race cancels
    io_uring_prep_cancel64(sqe, op.original_ud, 0);
    io_uring_sqe_set_data(sqe, nullptr);
}

void IoWorker::drain_local()
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

void IoWorker::tick() noexcept
{
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
        io_uring_cqe* waited_cqe = nullptr;
        __kernel_timespec timeout{.tv_sec = opts_.tick_timeout_ms / 1000,
                                  .tv_nsec = (opts_.tick_timeout_ms % 1000) * 1'000'000};

        if (const auto ret = io_uring_submit_and_wait_timeout(&ring_, &waited_cqe, 1, &timeout, nullptr);
            ret < 0 && ret != -EINTR && ret != -ETIME)
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

        // The target thread receives a ring message
        if ((ud & kRemoteTagMask) == kRemoteTaskTag)
        {
            const auto ptr = reinterpret_cast<void*>(ud & ~kRemoteTagMask);
            auto handle = std::coroutine_handle<>::from_address(ptr);
            ready_queue_.push_back(handle);
            continue;
        }

        // The SENDER thread receives confirmation of delivery
        if ((ud & kRemoteTagMask) == kRemoteSendTag)
        {
            // If the kernel failed to deliver (e.g. -EOVERFLOW, -EBADFD)
            if (cqe->res < 0)
            {
                ALOG_ERROR("msg_ring delivery failed: {}", std::strerror(-cqe->res));
                const auto ptr = reinterpret_cast<void*>(ud & ~kRemoteTagMask);
                auto handle = std::coroutine_handle<>::from_address(ptr);
                // Destroy the leaked frame
                handle.destroy();
            }
            // If cqe->res == 0, delivery was successful. We do nothing because
            // the target thread now owns the pointer.
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

void IoWorker::run(InternalKey, const std::stop_token st) noexcept
{
    // set the current io
    tl_io = this;

    // owner thread should be set on the thread which start the loop
    owner_thread_ = std::this_thread::get_id();

    // 128-byte frames: 64 preallocated
    // 256-byte frames: most common
    // 512-byte frames: combinators
    CoroAllocator::prewarm(0, 128);
    CoroAllocator::prewarm(1, 256);
    CoroAllocator::prewarm(2, 64);

    while (!st.stop_requested())
    {
        tick();
    }

    if (ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&ring_);
        ring_.ring_fd = -1;
    }

    ready_queue_.clear();
    process_queue_.clear();
    op_pool_.destroy_active_handles();

    // reset the tls context
    tl_io = nullptr;
    CoroAllocator::cleanup_thread_cache();
}

IoWorker::~IoWorker()
{
    if (ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&ring_);
        ring_.ring_fd = -1;
    }
}

//
// OP Context
//
IoContext::IoContext(const std::size_t num_threads, const IoOptions opts) : num_threads_(num_threads), opts_(opts)
{
    contexts_.reserve(num_threads);
    workers_.reserve(num_threads);

    if (num_threads == 0)
    {
        throw std::runtime_error("io context started with 0 thread");
    }
}

IoContext::~IoContext()
{
    stop();
    join();
}
}  // namespace URing
