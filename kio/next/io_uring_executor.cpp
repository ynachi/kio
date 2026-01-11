//
// Created by Yao ACHI on 11/01/2026.
//
#include "io_uring_executor.h"

#include "kio/core/async_logger.h"

#include <iostream>
#include <stdexcept>

#include <pthread.h>

#include <sys/poll.h>

namespace kio::next
{
// Thread-local storage
thread_local IoUringExecutor::PerThreadContext* IoUringExecutor::current_context_ = nullptr;

// Special marker for eventfd completions
static constexpr uint64_t kEventfdMarker = 0xFFFFFFFFFFFFFFFFULL;

IoUringExecutor::IoUringExecutor(const IoUringExecutorConfig& config)
{
    if (config.num_threads == 0)
    {
        throw std::invalid_argument("num_threads must be > 0");
    }

    size_t queue_size = config.task_queue_size;

    contexts_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        auto ctx = std::make_unique<PerThreadContext>(i, queue_size);

        int ret = io_uring_queue_init(config.io_uring_entries, &ctx->ring, config.io_uring_flags);
        if (ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init failed");
        }

        ctx->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (ctx->eventfd < 0)
        {
            throw std::system_error(errno, std::system_category(), "eventfd creation failed");
        }

        contexts_.push_back(std::move(ctx));
    }

    threads_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        threads_.emplace_back(
            [this, i, &config]()
            {
                auto* ctx = contexts_[i].get();
                current_context_ = ctx;
                ctx->thread_id = std::this_thread::get_id();

                if (config.pin_threads)
                {
                    pinThreadToCpu(i, i);
                }

                runEventLoop(ctx);
            });
    }
}

IoUringExecutor::~IoUringExecutor()
{
    stop();
    join();

    // Cleanup io_uring instances
    for (auto& ctx : contexts_)
    {
        io_uring_queue_exit(&ctx->ring);
    }
}

void IoUringExecutor::stop()
{
    stopped_.store(true, std::memory_order_release);

    // Wake all threads
    for (auto& ctx : contexts_)
    {
        ctx->running.store(false, std::memory_order_release);
        wakeThread(*ctx);
    }
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

// Fixed: Signature matches base class (by value)
bool IoUringExecutor::schedule(Func func)
{
    if (stopped_.load(std::memory_order_acquire))
    {
        return false;
    }

    auto& ctx = selectContext();
    return scheduleOn(ctx.context_id, std::move(func));
}

bool IoUringExecutor::scheduleOn(size_t context_id, Func&& func)
{
    if (context_id >= contexts_.size() || stopped_.load(std::memory_order_acquire))
    {
        return false;
    }

    auto& ctx = *contexts_[context_id];

    if (!ctx.task_queue.TryPush(std::move(func)))
    {
        return false;
    }

    ctx.task_count.fetch_add(1, std::memory_order_relaxed);

    if (current_context_ != &ctx)
    {
        wakeThread(ctx);
    }
    return true;
}

bool IoUringExecutor::scheduleLocal(Func&& func)
{
    if (current_context_)
    {
        return scheduleOn(current_context_->context_id, std::move(func));
    }
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
    // Register eventfd for wake-ups using multishot poll (if available)
    io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (sqe)
    {
        io_uring_prep_poll_add(sqe, ctx->eventfd, POLLIN);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(kEventfdMarker));
        io_uring_submit(&ctx->ring);
    }

    while (ctx->running.load(std::memory_order_acquire))
    {
        // Process local task queue
        if (processLocalQueue(ctx))
        {
            continue;  // if we processed a full batch, check again immediately
        }

        // Wait for I/O completions (with timeout to periodically check running flag)
        struct __kernel_timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000000;  // 100ms

        io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, &ts);

        if (ret == -ETIME)
        {
            // Timeout, just loop again
            continue;
        }

        if (ret == 0)
        {
            // Process all available completions
            unsigned head;
            unsigned count = 0;

            io_uring_for_each_cqe(&ctx->ring, head, cqe)
            {
                handleCompletion(ctx, cqe);
                count++;
            }

            io_uring_cq_advance(&ctx->ring, count);
        }
    }
}

bool IoUringExecutor::processLocalQueue(PerThreadContext* ctx)
{
    Task task;
    constexpr size_t kMaxBatchSize = 64;
    size_t processed = 0;

    while (processed < kMaxBatchSize && ctx->task_queue.TryPop(task))
    {
        try
        {
            task();
        }
        catch (const std::exception& e)
        {
            std::cerr << "Exception in task: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Unknown exception in task" << std::endl;
        }
        processed++;
    }

    return processed == kMaxBatchSize;
}

void IoUringExecutor::wakeThread(PerThreadContext& ctx)
{
    uint64_t val = 1;
    // Ignore errors - if write fails, the thread will wake up eventually
    [[maybe_unused]] ssize_t _ = write(ctx.eventfd, &val, sizeof(val));
}

IoUringExecutor::PerThreadContext& IoUringExecutor::selectContext()
{
    // Always round-robin for general scheduling to ensure load balancing
    size_t idx = next_context_.fetch_add(1, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[idx];
}

void IoUringExecutor::pinThreadToCpu(size_t thread_id, size_t cpu_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
    {
        std::cerr << "Worker Failed to set CPU affinity: " << strerror(rc) << std::endl;
    }

    if (const int ret = io_uring_register_iowq_aff(&current_context_->ring, 1, &cpuset); ret < 0)
    {
        // Warn only, this affects internal workers
        // std::cerr << "Worker : Failed to set io-wq affinity: " << strerror(-ret) << std::endl;
    }
}

void IoUringExecutor::handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe)
{
    void* user_data = io_uring_cqe_get_data(cqe);

    // Check if this is an eventfd notification
    if (user_data == reinterpret_cast<void*>(kEventfdMarker))
    {
        // Clear the eventfd
        uint64_t val;
        [[maybe_unused]] ssize_t _ = read(ctx->eventfd, &val, sizeof(val));

        // Re-arm the eventfd poll
        io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
        if (sqe)
        {
            io_uring_prep_poll_add(sqe, ctx->eventfd, POLLIN);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(kEventfdMarker));
            io_uring_submit(&ctx->ring);
        }
        return;
    }

    // This is an I/O operation completion
    if (user_data != nullptr)
    {
        auto* completion = static_cast<IoCompletion*>(user_data);
        if (completion->complete)
        {
            completion->complete(completion->self, cqe->res);
        }
    }
}
}  // namespace kio::next