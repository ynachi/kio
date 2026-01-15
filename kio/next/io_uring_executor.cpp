//
// io_uring_executor.cpp
// Production-Ready Implementation
//

#include "io_uring_executor.h"

#include <latch>
#include <stdexcept>

#include <fcntl.h>
#include <pthread.h>

namespace kio::next::v1
{

thread_local IoUringExecutor::PerThreadContext* IoUringExecutor::current_context_ = nullptr;

void IoUringExecutor::TaskOp::returnToPool()
{
    // Same thread: direct release to local pool
    if (source_ctx == nullptr || source_ctx == owner_ctx)
    {
        if (owner_ctx != nullptr && owner_ctx->task_pool)
        {
            owner_ctx->task_pool->release(this);
        }
        else
        {
            // Context shutting down or invalid
            delete this;
        }
        return;
    }

    // Cross-thread: send back via msg_ring to the source thread
    io_uring* control = owner_ctx->control_ring.get();
    io_uring_sqe* sqe = io_uring_get_sqe(control);

    if (sqe == nullptr)
    {
        io_uring_submit(control);
        sqe = io_uring_get_sqe(control);
    }

    if (sqe == nullptr)
    {
        // Ring full and submit failed to clear space.
        // We cannot block here. Leak prevention via deletion is safe but costly.
        delete this;
        return;
    }

    // Mark as pool return
    type = Type::PoolReturn;

    const int target_fd = source_ctx->ring.fd();

    // Send pointer as user_data. len=0.
    io_uring_prep_msg_ring(sqe, target_fd, 0, reinterpret_cast<__u64>(static_cast<IoOp*>(this)), 0);
    io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);

    // We do NOT call io_uring_submit here to batch returns implicitly.
    // The event loop will submit eventually.
}

void IoUringExecutor::validateConfig()
{
    if (executor_config_.num_threads == 0)
    {
        throw std::invalid_argument("num_threads must be > 0");
    }

    if (executor_config_.max_cqe_batch == 0)
    {
        throw std::invalid_argument("max_cqe_batch must be > 0");
    }
}

void IoUringExecutor::initExternalControlRing()
{
    io_uring_params params{};
    params.flags = 0;
    if (const int ret = external_control_ring_.init(executor_config_.control_ring_entries, &params); ret < 0)
    {
        throw std::system_error(-ret, std::system_category(), "external control ring init failed");
    }
}

void IoUringExecutor::registerPendingIo(PendingIo* op)
{
    pending_io_count_.fetch_add(1, std::memory_order_relaxed);
    if (!executor_config_.cancel_on_stop)
    {
        return;
    }

    std::scoped_lock lock(pending_io_mutex_);
    (void)pending_io_.insert(op);
}

void IoUringExecutor::unregisterPendingIo(PendingIo* op)
{
    pending_io_count_.fetch_sub(1, std::memory_order_relaxed);
    if (!executor_config_.cancel_on_stop)
    {
        return;
    }

    std::scoped_lock lock(pending_io_mutex_);
    (void)pending_io_.erase(op);
}

void IoUringExecutor::requestCancelPendingIo()
{
    if (!executor_config_.cancel_on_stop)
    {
        return;
    }

    std::vector<PendingIo*> snapshot;
    {
        std::scoped_lock lock(pending_io_mutex_);
        snapshot.reserve(pending_io_.size());
        for (auto* op : pending_io_)
        {
            snapshot.push_back(op);
        }
    }

    for (auto* op : snapshot)
    {
        op->requestCancel();
    }
}

bool IoUringExecutor::shouldDrain() const noexcept
{
    return drain_on_stop_.load(std::memory_order_acquire) && pending_io_count_.load(std::memory_order_acquire) > 0;
}

void IoUringExecutor::workerThreadEntry(size_t index, std::shared_ptr<std::latch> init_latch,
                                        std::atomic<bool>& init_failed, std::atomic<int>& init_error)
{
    auto* ctx = contexts_[index].get();
    current_context_ = ctx;
    ctx->thread_id = std::this_thread::get_id();
    ctx->stop_token = getStopToken();

    // 1. Worker Ring: Handles IO and incoming tasks
    {
        io_uring_params params{};
        params.flags = executor_config_.io_uring_flags | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN |
                       IORING_SETUP_DEFER_TASKRUN;

        int ret = ctx->ring.init(executor_config_.io_uring_entries, &params);
        if (ret < 0)
        {
            init_failed.store(true, std::memory_order_release);
            init_error.store(ret, std::memory_order_release);
            init_latch->count_down();
            return;
        }
    }

    // 2. Control Ring: Sends outbound tasks/messages
    {
        io_uring_params params{};
        params.flags = IORING_SETUP_SINGLE_ISSUER;

        int ret = ctx->control_ring.init(executor_config_.control_ring_entries, &params);
        if (ret < 0)
        {
            init_failed.store(true, std::memory_order_release);
            init_error.store(ret, std::memory_order_release);
            init_latch->count_down();
            return;
        }
    }

    if (executor_config_.pin_threads)
        pinThreadToCpu(index, index);

    init_latch->count_down();
    init_latch->wait();

    if (init_failed.load(std::memory_order_acquire))
    {
        ctx->setStopped();
        stop_latch_.count_down();
        return;
    }

    runEventLoop(ctx);
}

IoUringExecutor::IoUringExecutor(const IoUringExecutorConfig& config)
    : stop_latch_(config.num_threads), executor_config_(config)
{
    validateConfig();

    contexts_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        contexts_.push_back(std::make_unique<PerThreadContext>(i, config.task_pool_size));
    }

    initExternalControlRing();

    auto init_latch = std::make_shared<std::latch>(config.num_threads);
    std::atomic<bool> init_failed{false};
    std::atomic<int> init_error{0};

    threads_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        threads_.emplace_back(&IoUringExecutor::workerThreadEntry, this, i, init_latch, std::ref(init_failed),
                              std::ref(init_error));
    }

    init_latch->wait();

    if (init_failed.load(std::memory_order_acquire))
    {
        (void)stop_source_.request_stop();
        join();
        throw std::system_error(-init_error.load(), std::system_category(), "io_uring worker ring init failed");
    }
}

IoUringExecutor::~IoUringExecutor()
{
    if (!executor_stopped_.load(std::memory_order_acquire))
    {
        stop();
        join();
    }
}

void IoUringExecutor::stop()
{
    drain_on_stop_.store(true, std::memory_order_release);
    if (!stop_source_.request_stop())
    {
        return;
    }

    requestCancelPendingIo();

    // Wake up all threads using external control ring
    {
        std::scoped_lock lock(external_control_mutex_);

        for (auto& ctx : contexts_)
        {
            if (!ctx->ring.initialized())
            {
                continue;
            }

            io_uring_sqe* sqe = io_uring_get_sqe(external_control_ring_.get());
            if (sqe == nullptr)
            {
                (void)io_uring_submit(external_control_ring_.get());
                sqe = io_uring_get_sqe(external_control_ring_.get());
                if (sqe == nullptr)
                {
                    continue;
                }
            }

            int const target_fd = ctx->ring.fd();
            io_uring_prep_msg_ring(sqe, target_fd, 0, 0, 0);
            io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
        }
        io_uring_submit(external_control_ring_.get());
    }

    stop_latch_.wait();
    executor_stopped_.store(true, std::memory_order_release);
}

void IoUringExecutor::join()
{
    for (auto& thread : threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

bool IoUringExecutor::schedule(Func func)
{
    if (executor_stopped_.load(std::memory_order_acquire))
    {
        return false;
    }

    auto& ctx = selectContext();
    return scheduleOn(ctx.context_id, InternalFunc(std::move(func)));
}

bool IoUringExecutor::scheduleOn(size_t context_id, InternalFunc func)
{
    if (context_id >= contexts_.size() || executor_stopped_.load(std::memory_order_acquire))
    {
        return false;
    }

    if (executor_config_.enable_metrics)
    {
        metrics_.tasks_scheduled.fetch_add(1, std::memory_order_relaxed);
    }

    return submitTask(context_id, std::move(func));
}

bool IoUringExecutor::scheduleLocal(InternalFunc func)
{
    if (current_context_ != nullptr)
    {
        return scheduleOn(current_context_->context_id, std::move(func));
    }

    auto& ctx = selectContext();
    return scheduleOn(ctx.context_id, std::move(func));
}

bool IoUringExecutor::currentThreadInExecutor() const
{
    return current_context_ != nullptr;
}

size_t IoUringExecutor::currentContextId() const
{
    return (current_context_ != nullptr) ? current_context_->context_id : 0;
}

IoUringExecutor::Context IoUringExecutor::checkout()
{
    return (current_context_ != nullptr) ? static_cast<Context>(current_context_) : NULLCTX;
}

bool IoUringExecutor::checkin(Func func, Context ctx, async_simple::ScheduleOptions)
{
    if (ctx == NULLCTX)
    {
        return schedule(std::move(func));
    }

    auto* pctx = static_cast<PerThreadContext*>(ctx);
    return scheduleOn(pctx->context_id, InternalFunc(std::move(func)));
}

size_t IoUringExecutor::pickContextId()
{
    return selectContext().context_id;
}

void IoUringExecutor::runEventLoop(PerThreadContext* ctx)
{
    io_uring* ring = ctx->ring.get();
    io_uring_cqe* cqe = nullptr;

    const size_t max_batch = executor_config_.max_cqe_batch;

    while (!ctx->stop_token.stop_requested() || shouldDrain())
    {
        bool processed = false;
        size_t batch_count = 0;

        // 1. Process Worker Ring (Inbound Tasks & IO)
        while (batch_count < max_batch && io_uring_peek_cqe(ring, &cqe) == 0)
        {
            if (batch_count % 32 == 0 && ctx->stop_token.stop_requested())
            {
                break;
            }

            bool is_task = false;
            handleCompletion(ctx, cqe, is_task);
            io_uring_cqe_seen(ring, cqe);
            processed = true;
            batch_count++;

            if (executor_config_.enable_metrics)
            {
                if (is_task)
                {
                    metrics_.tasks_completed.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    metrics_.io_ops_completed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // 2. Process Control Ring (Outbound Acks & Errors)
        // We must reap this to handle msg_ring failures and prevent CQ overflow.
        {
            io_uring_cqe* ccqe = nullptr;
            unsigned head;
            unsigned c_count = 0;
            io_uring_for_each_cqe(ctx->control_ring.get(), head, ccqe)
            {
                c_count++;
                // If result < 0, the msg_ring failed (e.g., target ring full).
                // We must recover the Op to prevent memory leaks or lost tasks.
                if (ccqe->res < 0)
                {
                    auto* op = reinterpret_cast<IoOp*>(io_uring_cqe_get_data(ccqe));
                    if (op)
                    {
                        if (op->type == IoOp::Type::Task)
                        {
                            // Failed to send task to target thread.
                            // Fallback: Run locally to ensure progress, then reclaim.
                            auto* task = static_cast<TaskOp*>(op);
                            if (task && task->fn)
                            {
                                task->fn();
                            }
                            // Since we failed to send it, we (the source) still own the memory
                            // and it came from our pool.
                            if (ctx->task_pool)
                            {
                                ctx->task_pool->release(task);
                            }
                            else
                            {
                                delete task;
                            }
                        }
                        else if (op->type == IoOp::Type::PoolReturn)
                        {
                            // Failed to return object to its source thread.
                            // We cannot reclaim it into our pool (wrong owner).
                            // We must delete it to prevent leak.
                            auto* task = static_cast<TaskOp*>(op);
                            delete task;
                        }
                    }
                }
            }
            if (c_count > 0)
            {
                io_uring_cq_advance(ctx->control_ring.get(), c_count);
            }
        }

        if (executor_config_.enable_metrics && processed)
        {
            metrics_.batches_processed.fetch_add(1, std::memory_order_relaxed);
        }

        // Submit pending work on both rings
        io_uring_submit(ring);
        io_uring_submit(ctx->control_ring.get());

        if (!processed)
        {
            // Wait for work
            if (int const ret = io_uring_wait_cqe(ring, &cqe); ret == 0)
            {
                bool is_task = false;
                handleCompletion(ctx, cqe, is_task);
                io_uring_cqe_seen(ring, cqe);

                if (executor_config_.enable_metrics)
                {
                    if (is_task)
                    {
                        metrics_.tasks_completed.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        metrics_.io_ops_completed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    // Cleanup phase
    ctx->task_pool->shutdown();
    ctx->setStopped();
    stop_latch_.count_down();
}

IoUringExecutor::PerThreadContext& IoUringExecutor::selectContext()
{
    size_t const idx = next_context_.fetch_add(1, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[idx];
}

void IoUringExecutor::pinThreadToCpu(size_t /*thread_id*/, size_t cpu_id)
{
    if (cpu_id >= CPU_SETSIZE)
    {
        return;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    if ((current_context_ != nullptr) && current_context_->ring.initialized())
    {
        io_uring_register_iowq_aff(current_context_->ring.get(), 1, &cpuset);
    }
}

void IoUringExecutor::handleCompletion(PerThreadContext* /*ctx*/, io_uring_cqe* cqe, bool& is_task)
{
    void* user_data = io_uring_cqe_get_data(cqe);
    is_task = false;

    if (user_data != nullptr)
    {
        auto* op = static_cast<IoOp*>(user_data);

        if (op->type == IoOp::Type::PoolReturn)
        {
            if (executor_config_.enable_metrics)
            {
                metrics_.pool_returns_via_ring.fetch_add(1, std::memory_order_relaxed);
            }

            auto* task = dynamic_cast<TaskOp*>(op);
            task->reclaim();
            return;
        }
        is_task = (op->type == IoOp::Type::Task);
        op->complete(cqe->res);
    }
}

bool IoUringExecutor::submitTask(const size_t context_id, InternalFunc func)
{
    if ((current_context_ != nullptr) && current_context_->context_id == context_id)
    {
        if (executor_config_.enable_metrics)
        {
            metrics_.same_thread_schedules.fetch_add(1, std::memory_order_relaxed);
        }
        return submitTaskSameThread(current_context_, std::move(func));
    }

    if (current_context_ != nullptr)
    {
        if (executor_config_.enable_metrics)
        {
            metrics_.cross_thread_schedules.fetch_add(1, std::memory_order_relaxed);
        }
        return submitTaskCrossThread(context_id, std::move(func));
    }

    if (executor_config_.enable_metrics)
    {
        metrics_.external_schedules.fetch_add(1, std::memory_order_relaxed);
    }
    return submitTaskExternal(context_id, std::move(func));
}

bool IoUringExecutor::submitTaskSameThread(PerThreadContext* ctx, InternalFunc func)
{
    TaskOp* task = ctx->task_pool->acquire(std::move(func));
    task->owner_ctx = ctx;
    task->source_ctx = ctx;

    io_uring* ring = ctx->ring.get();
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    if (sqe == nullptr)
    {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (sqe == nullptr)
        {
            ctx->task_pool->release(task);
            return false;
        }
    }

    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, task);

    if (executor_config_.immediate_submit)
    {
        io_uring_submit(ring);
    }

    return true;
}

bool IoUringExecutor::submitTaskCrossThread(size_t context_id, InternalFunc func)
{
    auto* target_ctx = contexts_[context_id].get();

    // Allocate from SOURCE thread's pool
    TaskOp* task = current_context_->task_pool->acquire(std::move(func));
    task->owner_ctx = target_ctx;
    task->source_ctx = current_context_;

    io_uring* control = current_context_->control_ring.get();
    io_uring_sqe* sqe = io_uring_get_sqe(control);

    if (sqe == nullptr)
    {
        io_uring_submit(control);
        sqe = io_uring_get_sqe(control);
        if (sqe == nullptr)
        {
            current_context_->task_pool->release(task);
            return false;
        }
    }

    int target_fd = target_ctx->ring.fd();

    // Send pointer via user_data
    io_uring_prep_msg_ring(sqe, target_fd, 0, reinterpret_cast<__u64>(static_cast<IoOp*>(task)), 0);
    io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
    io_uring_submit(control);  // Always submit cross-thread messages immediately to reduce latency

    return true;
}

bool IoUringExecutor::submitTaskExternal(size_t context_id, InternalFunc func)
{
    auto* target_ctx = contexts_[context_id].get();

    auto* task = new TaskOp(std::move(func));
    task->owner_ctx = target_ctx;
    task->source_ctx = nullptr;

    std::scoped_lock lock(external_control_mutex_);

    // Drain potential errors from the external control ring to prevent overflow
    {
        io_uring* ext_ring = external_control_ring_.get();
        io_uring_cqe* ccqe = nullptr;
        unsigned head;
        unsigned c_count = 0;
        io_uring_for_each_cqe(ext_ring, head, ccqe)
        {
            c_count++;
            if (ccqe->res < 0)
            {
                auto* op = reinterpret_cast<IoOp*>(io_uring_cqe_get_data(ccqe));
                if (op)
                {
                    // External submissions are always Type::Task and allocated via new.
                    // If they fail, we must delete them.
                    if (op->type == IoOp::Type::Task)
                    {
                        auto* failed_task = static_cast<TaskOp*>(op);
                        delete failed_task;
                    }
                }
            }
        }
        if (c_count > 0)
        {
            io_uring_cq_advance(ext_ring, c_count);
        }
    }

    io_uring_sqe* sqe = io_uring_get_sqe(external_control_ring_.get());
    if (sqe == nullptr)
    {
        io_uring_submit(external_control_ring_.get());
        sqe = io_uring_get_sqe(external_control_ring_.get());
        if (sqe == nullptr)
        {
            delete task;
            return false;
        }
    }

    const int target_fd = target_ctx->ring.fd();
    io_uring_prep_msg_ring(sqe, target_fd, 0, reinterpret_cast<__u64>(static_cast<IoOp*>(task)), 0);
    io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
    io_uring_submit(external_control_ring_.get());

    return true;
}

}  // namespace kio::next::v1