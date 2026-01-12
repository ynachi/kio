//
// Created by Yao ACHI on 11/01/2026.
//
#include "io_uring_executor.h"

#include "kio/core/async_logger.h"

#include <format>
#include <stdexcept>

#include <pthread.h>

#include <sys/poll.h>

#include <ylt/easylog.hpp>

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

    // Create contexts
    contexts_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        auto ctx = std::make_unique<PerThreadContext>(i);

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

    // IMPORTANT:
    // async_simple::Lazy::via() will call executor->schedule() from inside worker threads.
    // If we round-robin here, a continuation can run on another worker and destroy helper
    // coroutine frames concurrently -> TSAN races in ViaCoroutine / std::function.
    if (current_context_ != nullptr)
    {
        return scheduleOn(current_context_->context_id, std::move(func));
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
    if (!ctx.task_queue.enqueue(std::move(func)))
    {
        return false;  // Queue full
    }

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
    // Register eventfd for wake-ups using a multishot poll
    if (io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring))
    {
        io_uring_prep_poll_multishot(sqe, ctx->eventfd, POLLIN);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(EVENTFD_MARKER));
        io_uring_submit(&ctx->ring);
    }

    while (ctx->running.load(std::memory_order_acquire))
    {
        // Process the local task queue
        processLocalQueue(ctx);

        if (const auto ret = io_uring_submit(&ctx->ring); ret < 0)
        {
            ELOGFMT(ERROR, "io_uring_submit failed: {}", strerror(-ret))
        }

        io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe(&ctx->ring, &cqe);

        if (ret == -ETIME)
        {
            // Timeout, just loop again
            continue;
        }

        if (ret < 0)
        {
            ELOGFMT(ERROR, "io_uring_wait_cqe failed: {}", strerror(-ret))
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

    while (processed < kMaxBatchSize && ctx->task_queue.try_dequeue(task))
    {
        try
        {
            task();
        }
        catch (const std::exception& e)
        {
            ELOGFMT(ERROR, "Exception in task: {}", e.what())
        }
        catch (...)
        {
            ELOGFMT(ERROR, "Unknown exception in task")
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
    // Round-robin selection
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
        ELOGFMT(WARNING, "Worker {}: Failed to set CPU affinity: {}", thread_id, strerror(rc))
    }

    if (const int ret = io_uring_register_iowq_aff(&current_context_->ring, 1, &cpuset); ret < 0)
    {
        ELOGFMT(WARNING, "Worker {}: Failed to set io-wq affinity: {}", thread_id, strerror(-ret))
        // ALOG_WARN("Worker : Failed to set io-wq affinity: {}", strerror(-ret));
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