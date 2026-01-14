// io_uring_executor_lockfree.cpp
// Lock-free implementation with per-thread control rings
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
        throw std::invalid_argument("num_threads must be > 0");

    if (config.max_cqe_batch == 0)
        throw std::invalid_argument("max_cqe_batch must be > 0");

    contexts_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        contexts_.push_back(std::make_unique<PerThreadContext>(i, config.task_pool_size));
    }

    // Initialize external control ring for scheduling from non-executor threads
    // This ring is protected by mutex since we don't know which thread will use it
    {
        io_uring_params params{};
        params.flags = 0;  // No SINGLE_ISSUER since multiple external threads may use it
        int ret = io_uring_queue_init_params(config.control_ring_entries, &external_control_ring_, &params);
        if (ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "external control ring init failed");
        }
    }

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

                // Initialize worker ring with SINGLE_ISSUER and cooperative task run
                {
                    io_uring_params params{};
                    params.flags = config.io_uring_flags | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN |
                                   IORING_SETUP_DEFER_TASKRUN;

                    int ret = io_uring_queue_init_params(config.io_uring_entries, &ctx->ring, &params);
                    if (ret < 0)
                    {
                        std::terminate();
                    }
                }

                // Initialize per-thread control ring with SINGLE_ISSUER
                // This thread is the ONLY issuer to this control ring
                {
                    io_uring_params params{};
                    params.flags = IORING_SETUP_SINGLE_ISSUER;

                    int ret = io_uring_queue_init_params(config.control_ring_entries, &ctx->control_ring, &params);
                    if (ret < 0)
                    {
                        std::terminate();
                    }
                }

                if (config.pin_threads)
                    pinThreadToCpu(i, i);

                init_latch->count_down();
                init_latch->wait();

                runEventLoop(ctx);
            });
    }

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
        io_uring_queue_exit(&ctx->control_ring);
    }
    io_uring_queue_exit(&external_control_ring_);
}

void IoUringExecutor::stop()
{
    if (!stop_source_.request_stop())
    {
        return;
    }

    // Wake all threads by sending dummy messages
    // Use external control ring since we're on main thread
    {
        std::lock_guard lock(external_control_mutex_);

        for (auto& ctx : contexts_)
        {
            io_uring_sqe* sqe = io_uring_get_sqe(&external_control_ring_);
            if (!sqe)
            {
                (void)io_uring_submit(&external_control_ring_);
                sqe = io_uring_get_sqe(&external_control_ring_);
                if (!sqe)
                    continue;
            }

            int target_fd = ctx->ring.ring_fd;
            io_uring_prep_msg_ring(sqe, target_fd, 0, 0, 0);
            io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
        }
        io_uring_submit(&external_control_ring_);
    }

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

    if (executor_config_.enable_metrics)
        metrics_.tasks_scheduled.fetch_add(1, std::memory_order_relaxed);

    return submitTask(context_id, std::move(func));
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

    const size_t max_batch = executor_config_.max_cqe_batch;

    while (!ctx->stop_token.stop_requested())
    {
        bool processed = false;
        size_t batch_count = 0;

        // Drain CQEs with batching limit
        while (batch_count < max_batch && io_uring_peek_cqe(ring, &cqe) == 0)
        {
            bool is_task = false;
            handleCompletion(ctx, cqe, is_task);
            io_uring_cqe_seen(ring, cqe);
            processed = true;
            batch_count++;

            if (executor_config_.enable_metrics)
            {
                if (is_task)
                    metrics_.tasks_completed.fetch_add(1, std::memory_order_relaxed);
                else
                    metrics_.io_ops_completed.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (executor_config_.enable_metrics && processed)
            metrics_.batches_processed.fetch_add(1, std::memory_order_relaxed);

        // Submit any pending SQEs on worker ring
        io_uring_submit(ring);

        // Also submit any pending SQEs on control ring
        // (in case we've queued cross-thread messages)
        io_uring_submit(&ctx->control_ring);

        if (!processed)
        {
            // Wait for the next completion
            int ret = io_uring_wait_cqe(ring, &cqe);
            if (ret == 0)
            {
                bool is_task = false;
                handleCompletion(ctx, cqe, is_task);
                io_uring_cqe_seen(ring, cqe);

                if (executor_config_.enable_metrics)
                {
                    if (is_task)
                        metrics_.tasks_completed.fetch_add(1, std::memory_order_relaxed);
                    else
                        metrics_.io_ops_completed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else if (ret == -EINTR)
            {
                continue;
            }
        }
    }

    ctx->setStopped();
    stop_latch_.count_down();
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

void IoUringExecutor::handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe, bool& is_task)
{
    void* user_data = io_uring_cqe_get_data(cqe);
    is_task = false;

    if (user_data)
    {
        auto* op = static_cast<IoOp*>(user_data);
        is_task = (dynamic_cast<TaskOp*>(op) != nullptr);
        op->complete(cqe->res);
    }
}

bool IoUringExecutor::submitTask(size_t context_id, Func func)
{
    // Same-thread optimization
    if (current_context_ && current_context_->context_id == context_id)
    {
        if (executor_config_.enable_metrics)
            metrics_.same_thread_schedules.fetch_add(1, std::memory_order_relaxed);
        return submitTaskSameThread(current_context_, std::move(func));
    }

    // Cross-thread scheduling from executor thread
    if (current_context_)
    {
        if (executor_config_.enable_metrics)
            metrics_.cross_thread_schedules.fetch_add(1, std::memory_order_relaxed);
        return submitTaskCrossThread(context_id, std::move(func));
    }

    // External thread (not in executor)
    if (executor_config_.enable_metrics)
        metrics_.external_schedules.fetch_add(1, std::memory_order_relaxed);
    return submitTaskExternal(context_id, std::move(func));
}

bool IoUringExecutor::submitTaskSameThread(PerThreadContext* ctx, Func func)
{
    // Same thread - use NOP on worker ring
    TaskOp* task = ctx->task_pool->acquire(std::move(func));
    task->owner_ctx = ctx;

    io_uring* ring = &ctx->ring;
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    if (!sqe)
    {
        (void)io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe)
        {
            ctx->task_pool->release(task);
            return false;
        }
    }

    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, static_cast<IoOp*>(task));
    io_uring_submit(ring);

    return true;
}

bool IoUringExecutor::submitTaskCrossThread(size_t context_id, Func func)
{
    // Cross-thread from executor thread
    // Use THIS thread's control ring to send message to target thread
    // This is LOCK-FREE because only this thread uses this control ring

    auto* target_ctx = contexts_[context_id].get();

    TaskOp* task = target_ctx->task_pool->acquire(std::move(func));
    task->owner_ctx = target_ctx;

    // Use current thread's control ring (SINGLE_ISSUER, no lock needed)
    io_uring* control = &current_context_->control_ring;

    io_uring_sqe* sqe = io_uring_get_sqe(control);
    if (!sqe)
    {
        (void)io_uring_submit(control);
        sqe = io_uring_get_sqe(control);
        if (!sqe)
        {
            target_ctx->task_pool->release(task);
            return false;
        }
    }

    int target_fd = target_ctx->ring.ring_fd;
    io_uring_prep_msg_ring(sqe, target_fd, 0, reinterpret_cast<__u64>(static_cast<IoOp*>(task)), 0);
    io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);

    // Submit happens in runEventLoop, but we can submit immediately for lower latency
    io_uring_submit(control);

    return true;
}

bool IoUringExecutor::submitTaskExternal(size_t context_id, Func func)
{
    // External thread (main thread, user thread, etc.)
    // Use shared external control ring with mutex protection

    auto* target_ctx = contexts_[context_id].get();

    TaskOp* task = target_ctx->task_pool->acquire(std::move(func));
    task->owner_ctx = target_ctx;

    // Need mutex because we don't know which external thread is calling
    std::lock_guard lock(external_control_mutex_);

    io_uring_sqe* sqe = io_uring_get_sqe(&external_control_ring_);
    if (!sqe)
    {
        (void)io_uring_submit(&external_control_ring_);
        sqe = io_uring_get_sqe(&external_control_ring_);
        if (!sqe)
        {
            target_ctx->task_pool->release(task);
            return false;
        }
    }

    int target_fd = target_ctx->ring.ring_fd;
    io_uring_prep_msg_ring(sqe, target_fd, 0, reinterpret_cast<__u64>(static_cast<IoOp*>(task)), 0);
    io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
    io_uring_submit(&external_control_ring_);

    return true;
}

}  // namespace kio::next::v1