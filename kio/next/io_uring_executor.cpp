//
// io_uring_executor.cpp
// Optimization: Check sq_ready before submitting to save syscalls
//

#include "io_uring_executor.h"
#include <format>
#include <stdexcept>
#include <pthread.h>
#include <cerrno>

namespace kio::next::v1
{

thread_local IoUringExecutor::PerThreadContext* IoUringExecutor::current_context_ = nullptr;

IoUringExecutor::IoUringExecutor(const IoUringExecutorConfig& config)
{
    if (config.num_threads == 0)
        throw std::invalid_argument("num_threads must be > 0");

    contexts_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        auto ctx = std::make_unique<PerThreadContext>(i);

        int ret = io_uring_queue_init(config.io_uring_entries, &ctx->ring, config.io_uring_flags);
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init failed");

        ctx->eventfd = eventfd(0, 0);
        if (ctx->eventfd < 0)
            throw std::system_error(errno, std::system_category(), "eventfd creation failed");

        ret = io_uring_register_eventfd(&ctx->ring, ctx->eventfd);
        if (ret < 0)
            throw std::system_error(-ret, std::system_category(), "io_uring_register_eventfd failed");

        contexts_.push_back(std::move(ctx));
    }

    threads_.reserve(config.num_threads);
    for (size_t i = 0; i < config.num_threads; ++i)
    {
        threads_.emplace_back([this, i, &config]()
        {
            auto* ctx = contexts_[i].get();
            current_context_ = ctx;
            ctx->thread_id = std::this_thread::get_id();

            if (config.pin_threads)
                pinThreadToCpu(i, i);

            runEventLoop(ctx);
        });
    }
}

IoUringExecutor::~IoUringExecutor()
{
    stop();
    join();
    for (auto& ctx : contexts_)
        io_uring_queue_exit(&ctx->ring);
}

void IoUringExecutor::stop()
{
    stopped_.store(true, std::memory_order_release);
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
        if (thread.joinable()) thread.join();
    }
}

bool IoUringExecutor::schedule(Func func)
{
    if (stopped_.load(std::memory_order_acquire)) return false;
    auto& ctx = selectContext();
    return scheduleOn(ctx.context_id, std::move(func));
}

bool IoUringExecutor::scheduleOn(size_t context_id, Func func)
{
    if (context_id >= contexts_.size() || stopped_.load(std::memory_order_acquire)) return false;
    auto& ctx = *contexts_[context_id];

    // 1. Enqueue task
    if (!ctx.task_queue.enqueue(std::move(func))) return false;

    // 2. Smart Wake: Only syscall if thread is actually sleeping.
    // Use SC fence to ensure enqueue is visible before checking sleeping.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (ctx.sleeping.load(std::memory_order_seq_cst))
    {
        wakeThread(ctx);
    }

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
    if (ctx == NULLCTX) return schedule(std::move(func));
    auto* pctx = static_cast<PerThreadContext*>(ctx);
    return scheduleOn(pctx->context_id, std::move(func));
}

size_t IoUringExecutor::pickContextId()
{
    return selectContext().context_id;
}

void IoUringExecutor::runEventLoop(PerThreadContext* ctx)
{
    while (ctx->running.load(std::memory_order_acquire))
    {
        // 1. Process tasks (may be woken by task insertion)
        bool partial_drain = processLocalQueue(ctx);

        // 2. Submit IO (OPTIMIZED)
        // Only pay the syscall cost if we actually prepared some SQEs
        if (io_uring_sq_ready(&ctx->ring) > 0)
        {
            io_uring_submit(&ctx->ring);
        }

        // 3. Reap completions
        unsigned head;
        io_uring_cqe* cqe;
        unsigned count = 0;

        // Peek at completions without syscall
        io_uring_for_each_cqe(&ctx->ring, head, cqe)
        {
            handleCompletion(ctx, cqe);
            count++;
        }
        if (count > 0) io_uring_cq_advance(&ctx->ring, count);

        // 4. Park/Sleep Logic
        // If we did work (tasks or IO), we loop to try to do more work (busy-waitish).
        // Only if we did NOTHING do we prepare to sleep.
        if (partial_drain || count > 0)
        {
             continue;
        }

        // 5. Safe Sleep Pattern
        ctx->sleeping.store(true, std::memory_order_seq_cst);

        // Double Check: Did a task arrive while we were deciding to sleep?
        if (processLocalQueue(ctx)) {
            ctx->sleeping.store(false, std::memory_order_relaxed);
            continue;
        }

        // Also double check IO before sleeping?
        // Technically io_uring_submit might be needed if we generated work in the check above,
        // but processLocalQueue loop handles that on next iteration.
        // We block on eventfd.

        uint64_t val;
        ssize_t r = read(ctx->eventfd, &val, sizeof(val));
        (void)r;

        ctx->sleeping.store(false, std::memory_order_relaxed);
    }
}

bool IoUringExecutor::processLocalQueue(PerThreadContext* ctx)
{
    Task task;
    constexpr size_t kMaxBatchSize = 128;
    size_t processed = 0;

    while (processed < kMaxBatchSize && ctx->task_queue.try_dequeue(task))
    {
        try
        {
            task();
        }
        catch (...) {}
        processed++;
    }

    return processed == kMaxBatchSize;
}

void IoUringExecutor::wakeThread(PerThreadContext& ctx)
{
    uint64_t val = 1;
    ssize_t ret = write(ctx.eventfd, &val, sizeof(val));
    (void)ret;
}

IoUringExecutor::PerThreadContext& IoUringExecutor::selectContext()
{
    size_t idx = next_context_.fetch_add(1, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[idx];
}

void IoUringExecutor::pinThreadToCpu(size_t thread_id, size_t cpu_id)
{
    if (cpu_id >= CPU_SETSIZE) return;
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