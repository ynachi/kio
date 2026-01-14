//
// io_uring_executor.cpp
//
#include "io_uring_executor.h"

#include <cerrno>
#include <latch>
#include <stdexcept>

#include <fcntl.h>
#include <pthread.h>

namespace kio::next::v1
{

thread_local IoUringExecutor::PerThreadContext* IoUringExecutor::current_context_ = nullptr;

IoUringExecutor::IoUringExecutor(const IoUringExecutorConfig& config)
    : stop_latch_(config.num_threads), executor_config_(config)
{
    if (config.num_threads == 0)
        throw std::invalid_argument("num_threads > 0");

    contexts_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        // eventfd is created inside PerThreadContext constructor here (Main Thread).
        // This eliminates the race where a thread tries to wake this context
        // before the worker thread has started and created the fd.
        contexts_.push_back(std::make_unique<PerThreadContext>(i));
    }

    // Initialize a dedicated control ring for cross‑thread scheduling.  This ring
    // will be used exclusively by the main thread to issue IORING_OP_MSG_RING
    // operations.  Use SINGLE_ISSUER to guarantee thread safety on this ring.
    {
        io_uring_params params{};
        params.flags = IORING_SETUP_SINGLE_ISSUER;
        int ret = io_uring_queue_init_params(config.io_uring_entries, &control_ring_, &params);
        if (ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "control ring init failed");
        }
    }

    // FIX: Use shared_ptr for latch to prevent use-after-free
    // If the main thread wakes up and exits constructor, stack latch is destroyed
    // while worker threads might still be waiting/accessing it.
    auto init_latch = std::make_shared<std::latch>(config.num_threads);

    threads_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        threads_.emplace_back(
            [this, i, config, init_latch]()
            {
                auto* ctx = contexts_[i].get();
                current_context_ = ctx;
                ctx->thread_id = std::this_thread::get_id();
                ctx->stop_token = getStopToken();

                // Initialize per‑thread ring with single issuer and cooperative task run.
                io_uring_params params{};
                params.flags = config.io_uring_flags | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN |
                               IORING_SETUP_DEFER_TASKRUN;
                int ret = io_uring_queue_init_params(config.io_uring_entries, &ctx->ring, &params);
                if (ret < 0)
                {
                    std::terminate();
                }

                if (config.pin_threads)
                    pinThreadToCpu(i, i);

                init_latch->count_down();
                init_latch->wait();

                runEventLoop(ctx);
            });
    }

    // Main thread waits for all workers to be ready
    init_latch->wait();
}

IoUringExecutor::~IoUringExecutor()
{
    if (executor_stopped_)
        return;
    stop();
    join();
    for (auto& ctx : contexts_)
    {
        io_uring_queue_exit(&ctx->ring);
    }
    io_uring_queue_exit(&control_ring_);
}

void IoUringExecutor::stop()
{
    // Request stop for all worker loops
    if (!stop_source_.request_stop())
    {
        // Already stopped/stopping
    }

    // Send a dummy message to each ring to wake up any waiting thread.  Use
    // IORING_OP_MSG_RING with a null user_data so that handleCompletion()
    // ignores it.  Use IOSQE_CQE_SKIP_SUCCESS so the control ring does not
    // receive a completion event for these messages.
    for (auto& ctx : contexts_)
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&control_ring_);
        if (!sqe)
        {
            (void)io_uring_submit(&control_ring_);
            sqe = io_uring_get_sqe(&control_ring_);
            if (!sqe)
                continue;
        }
        int target_fd = ctx->ring.ring_fd;
        io_uring_prep_msg_ring(sqe, target_fd, 0, 0, 0);
        io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
    }
    io_uring_submit(&control_ring_);

    stop_latch_.wait();
    executor_stopped_.store(true, std::memory_order_release);
}

void IoUringExecutor::join()
{
    for (auto& thread : threads_)
    {
        if (thread.joinable())
            thread.join();
    }
}

bool IoUringExecutor::schedule(Func func)
{
    if (executor_stopped_.load(std::memory_order_acquire))
        return false;
    auto& ctx = selectContext();
    return scheduleOn(ctx.context_id, std::move(func));
}

bool IoUringExecutor::scheduleOn(size_t context_id, Func func)
{
    if (context_id >= contexts_.size() || executor_stopped_.load(std::memory_order_acquire))
        return false;

    // Wrap the function in a TaskOp so it can be delivered via io_uring.
    struct TaskOp : public IoOp
    {
        Func fn;
        explicit TaskOp(Func&& f) : fn(std::move(f)) {}
        void complete(int) override
        {
            try
            {
                fn();
            }
            catch (...)
            {
            }
            delete this;
        }
    };

    // Allocate task on the heap.  It will delete itself when complete() is called.
    auto* task = new TaskOp(std::move(func));

    // If scheduling from within the same context, post a NOP directly on the ring.
    if (current_context_ && current_context_->context_id == context_id)
    {
        io_uring* ring = &current_context_->ring;
        io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe)
        {
            (void)io_uring_submit(ring);
            sqe = io_uring_get_sqe(ring);
            if (!sqe)
            {
                delete task;
                return false;
            }
        }
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, static_cast<IoOp*>(task));
        io_uring_submit(ring);
        return true;
    }

    // Otherwise, use the control ring to send a message to the target ring.
    io_uring_sqe* sqe = io_uring_get_sqe(&control_ring_);
    if (!sqe)
    {
        (void)io_uring_submit(&control_ring_);
        sqe = io_uring_get_sqe(&control_ring_);
        if (!sqe)
        {
            delete task;
            return false;
        }
    }
    int target_fd = contexts_[context_id]->ring.ring_fd;
    io_uring_prep_msg_ring(sqe, target_fd, 0, reinterpret_cast<__u64>(static_cast<IoOp*>(task)), 0);
    io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
    io_uring_submit(&control_ring_);
    return true;
}

bool IoUringExecutor::scheduleLocal(Func func)
{
    if (current_context_)
        return scheduleOn(current_context_->context_id, std::move(func));
    return schedule(std::move(func));
}

bool IoUringExecutor::currentThreadInExecutor() const
{
    return current_context_ != nullptr;
}
size_t IoUringExecutor::currentContextId() const
{
    return current_context_ ? current_context_->context_id : 0;
}
IoUringExecutor::Context IoUringExecutor::checkout()
{
    return current_context_ ? static_cast<Context>(current_context_) : NULLCTX;
}

bool IoUringExecutor::checkin(Func func, Context ctx, async_simple::ScheduleOptions)
{
    if (ctx == NULLCTX)
        return schedule(std::move(func));
    auto* pctx = static_cast<PerThreadContext*>(ctx);
    return scheduleOn(pctx->context_id, std::move(func));
}

size_t IoUringExecutor::pickContextId()
{
    return selectContext().context_id;
}

void IoUringExecutor::runEventLoop(PerThreadContext* ctx)
{
    io_uring* ring = &ctx->ring;
    io_uring_cqe* cqe;
    while (!ctx->stop_token.stop_requested())
    {
        bool processed = false;
        // Drain all ready CQEs
        while (io_uring_peek_cqe(ring, &cqe) == 0)
        {
            handleCompletion(ctx, cqe);
            io_uring_cqe_seen(ring, cqe);
            processed = true;
        }
        // Submit any pending SQEs
        io_uring_submit(ring);
        if (!processed)
        {
            // Wait for the next completion
            int ret = io_uring_wait_cqe(ring, &cqe);
            if (ret == 0)
            {
                handleCompletion(ctx, cqe);
                io_uring_cqe_seen(ring, cqe);
            }
        }
    }
    ctx->setStopped();
    stop_latch_.count_down();
}

bool IoUringExecutor::processLocalQueue(PerThreadContext* ctx, size_t batch)
{
    // Local queue processing has been replaced by io_uring task scheduling.  No
    // additional queue is used.
    (void)ctx;
    (void)batch;
    return false;
}

void IoUringExecutor::wakeThread(PerThreadContext& ctx)
{
    // No-op: wakeThread is obsolete in the ring-only design.  Scheduling
    // cross‑thread tasks uses msg_ring messages instead of eventfds.
    (void)ctx;
}

IoUringExecutor::PerThreadContext& IoUringExecutor::selectContext()
{
    size_t idx = next_context_.fetch_add(1, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[idx];
}

void IoUringExecutor::pinThreadToCpu(size_t thread_id, size_t cpu_id)
{
    if (cpu_id >= CPU_SETSIZE)
        return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    io_uring_register_iowq_aff(&current_context_->ring, 1, &cpuset);
}

void IoUringExecutor::handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe)
{
    void* user_data = io_uring_cqe_get_data(cqe);
    if (user_data)
    {
        auto* op = static_cast<IoOp*>(user_data);
        op->complete(cqe->res);
    }
}

}  // namespace kio::next::v1
