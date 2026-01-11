//
// Created by Yao ACHI on 11/01/2026.
//
#include "io_uring_executor.h"

#include <iostream>
#include <stdexcept>

#include <pthread.h>

#include <sys/poll.h>

namespace kio::next
{
// Thread-local storage
thread_local IoUringExecutor::PerThreadContext* IoUringExecutor::current_context_ = nullptr;

// Special marker for eventfd completions
constexpr uint64_t EVENTFD_MARKER = 0xFFFFFFFFFFFFFFFFULL;

IoUringExecutor::IoUringExecutor(const IoUringExecutorConfig& config)
{
    if (config.num_threads == 0)
    {
        throw std::invalid_argument("num_threads must be > 0");
    }

    // Ensure task queue size is power of 2
    size_t queue_size = NextPowerOf2(config.task_queue_size);

    // Create contexts
    contexts_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        auto ctx = std::make_unique<PerThreadContext>(i, queue_size);

        // Initialize io_uring
        int ret = io_uring_queue_init(config.io_uring_entries, &ctx->ring, config.io_uring_flags);
        if (ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init failed");
        }

        // Create eventfd for waking up the thread
        ctx->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (ctx->eventfd < 0)
        {
            throw std::system_error(errno, std::system_category(), "eventfd creation failed");
        }

        contexts_.push_back(std::move(ctx));
    }

    // Start worker threads
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

    // Try to push to the queue
    if (!ctx.task_queue.TryPush(std::forward<Func>(func)))
    {
        return false;  // Queue full
    }

    ctx.task_count.fetch_add(1, std::memory_order_relaxed);

    // Wake the thread
    wakeThread(ctx);
    return true;
}

bool IoUringExecutor::scheduleLocal(Func&& func)
{
    if (current_context_)
    {
        // We're in an executor thread, use the local queue directly
        return scheduleOn(current_context_->context_id, std::forward<Func>(func));
    }
    // Not in executor, use normal schedule
    return schedule(std::forward<Func>(func));
}

bool IoUringExecutor::currentThreadInExecutor() const
{
    return current_context_ != nullptr;
}

size_t IoUringExecutor::currentContextId() const
{
    return current_context_ ? current_context_->context_id : 0;
}

void IoUringExecutor::runEventLoop(PerThreadContext* ctx)
{
    // Register eventfd for wake-ups using multishot poll (if available)
    io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (sqe)
    {
        io_uring_prep_poll_add(sqe, ctx->eventfd, POLLIN);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(EVENTFD_MARKER));
        io_uring_submit(&ctx->ring);
    }

    while (ctx->running.load(std::memory_order_acquire))
    {
        // Process local task queue
        processLocalQueue(ctx);

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

void IoUringExecutor::processLocalQueue(PerThreadContext* ctx)
{
    Task task;

    // Drain the queue in batches to avoid starvation
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
}

void IoUringExecutor::wakeThread(PerThreadContext& ctx)
{
    uint64_t val = 1;
    // Ignore errors - if write fails, the thread will wake up eventually
    [[maybe_unused]] ssize_t _ = write(ctx.eventfd, &val, sizeof(val));
}

IoUringExecutor::PerThreadContext& IoUringExecutor::selectContext()
{
    // If we're already in an executor thread, prefer to use the same thread
    // if (current_context_)
    // {
    //     return *current_context_;
    // }

    // Round-robin selection
    size_t idx = next_context_.fetch_add(1, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[idx];
}

void IoUringExecutor::pinThreadToCpu(size_t thread_id, size_t cpu_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    int ret = pthread_setaffinity_np(threads_[thread_id].native_handle(), sizeof(cpu_set_t), &cpuset);
    if (ret != 0)
    {
        std::cerr << "Warning: Failed to pin thread " << thread_id << " to CPU " << cpu_id << std::endl;
    }
}

void IoUringExecutor::handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe)
{
    void* user_data = io_uring_cqe_get_data(cqe);

    // Check if this is an eventfd notification
    if (user_data == reinterpret_cast<void*>(EVENTFD_MARKER))
    {
        // Clear the eventfd
        uint64_t val;
        [[maybe_unused]] ssize_t _ = read(ctx->eventfd, &val, sizeof(val));

        // Re-arm the eventfd poll
        io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
        if (sqe)
        {
            io_uring_prep_poll_add(sqe, ctx->eventfd, POLLIN);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(EVENTFD_MARKER));
            io_uring_submit(&ctx->ring);
        }
        return;
    }

    // This is an I/O operation completion
    // The user_data points to the Awaiter (IoOp) itself.
    if (user_data != nullptr)
    {
        auto* op = static_cast<IoOp*>(user_data);
        op->complete(cqe->res);
        // Note: No 'delete op' here. The op is the Awaiter, which lives
        // on the coroutine frame.
    }
}
}  // namespace kio::next