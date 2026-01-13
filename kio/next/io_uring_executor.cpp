//
// io_uring_executor_FAST.cpp
// Simple, fast design - NO smart waking, NO cross-thread indirection
//

#include "io_uring_executor.h"

#include <cerrno>
#include <latch>
#include <stdexcept>

#include <pthread.h>

namespace kio::next::v1
{

thread_local IoUringExecutor::PerThreadContext* IoUringExecutor::current_context_ = nullptr;

IoUringExecutor::IoUringExecutor(const IoUringExecutorConfig& config)
    : stop_latch_(config.num_threads), executor_config_(config)
{
    if (config.num_threads == 0)
    {
        throw std::invalid_argument("num_threads must be > 0");
    }

    contexts_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        contexts_.push_back(std::make_unique<PerThreadContext>(i));
    }

    std::latch init_latch(config.num_threads);

    threads_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        threads_.emplace_back(
            [this, i, config, &init_latch]()
            {
                auto* ctx = contexts_[i].get();
                current_context_ = ctx;
                ctx->thread_id = std::this_thread::get_id();
                ctx->stop_token = getStopToken();

                // Initialize io_uring with SINGLE_ISSUER
                int ret = io_uring_queue_init(config.io_uring_entries, &ctx->ring, config.io_uring_flags);
                if (ret < 0)
                {
                    std::terminate();
                }

                ctx->eventfd = eventfd(0, 0);
                if (ctx->eventfd < 0)
                {
                    std::terminate();
                }

                ret = io_uring_register_eventfd(&ctx->ring, ctx->eventfd);
                if (ret < 0)
                {
                    std::terminate();
                }

                if (config.pin_threads)
                {
                    pinThreadToCpu(i, i);
                }

                // Signal ready and wait for all threads
                init_latch.count_down();
                init_latch.wait();

                // All threads ready - safe to run event loop
                runEventLoop(ctx);
            });
    }

    // Wait for all threads to initialize
    init_latch.wait();
}

IoUringExecutor::~IoUringExecutor()
{
    if (executor_stopped_)
    {
        return;
    }
    stop();
    join();
    for (auto& ctx : contexts_)
    {
        io_uring_unregister_eventfd(&ctx->ring);
        io_uring_queue_exit(&ctx->ring);
    }
}

void IoUringExecutor::stop()
{
    (void)stop_source_.request_stop();
    for (auto& ctx : contexts_)
    {
        wakeThread(*ctx);
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

    auto& ctx = *contexts_[context_id];

    // SIMPLE & FAST: No fences, no smart waking
    if (!ctx.task_queue.enqueue(std::move(func)))
        return false;

    wakeThread(ctx);
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
    {
        return schedule(std::move(func));
    }
    auto* pctx = static_cast<PerThreadContext*>(ctx);
    return scheduleOn(pctx->context_id, std::move(func));
}

size_t IoUringExecutor::pickContextId()
{
    return selectContext().context_id;
}

void IoUringExecutor::runEventLoop(PerThreadContext* ctx)
{
    while (!ctx->stop_token.stop_requested())
    {
        // Process task queue
        processLocalQueue(ctx, executor_config_.local_batch_size);

        // Check completions (non-blocking peek)
        io_uring_cqe* cqe;
        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ctx->ring, head, cqe)
        {
            handleCompletion(ctx, cqe);
            count++;
        }
        if (count > 0)
        {
            io_uring_cq_advance(&ctx->ring, count);
        }

        // Submit pending I/O
        io_uring_submit(&ctx->ring);

        // Wait for events
        uint64_t eventfd_value;
        ssize_t n = read(ctx->eventfd, &eventfd_value, sizeof(eventfd_value));
        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        // Process all completions after wake
        count = 0;
        io_uring_for_each_cqe(&ctx->ring, head, cqe)
        {
            handleCompletion(ctx, cqe);
            count++;
        }
        if (count > 0)
        {
            io_uring_cq_advance(&ctx->ring, count);
        }
    }

    // now mark as stopped
    ctx->setStopped();
    stop_latch_.count_down();
}

void IoUringExecutor::processLocalQueue(PerThreadContext* ctx, size_t batch)
{
    Task task;
    size_t processed = 0;

    while (processed < batch && ctx->task_queue.try_dequeue(task))
    {
        try
        {
            task();
        }
        catch (...)
        {
        }
        processed++;
    }
}

void IoUringExecutor::wakeThread(PerThreadContext& ctx)
{
    uint64_t val = 1;
    write(ctx.eventfd, &val, sizeof(val));
}

IoUringExecutor::PerThreadContext& IoUringExecutor::selectContext()
{
    size_t idx = next_context_.fetch_add(1, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[idx];
}

void IoUringExecutor::pinThreadToCpu(size_t thread_id, size_t cpu_id)
{
    if (cpu_id >= CPU_SETSIZE)
    {
        return;
    }
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