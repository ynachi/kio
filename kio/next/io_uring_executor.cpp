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

                int ret = io_uring_queue_init(config.io_uring_entries, &ctx->ring, config.io_uring_flags);
                if (ret < 0)
                    std::terminate();

                // FIX 1: Use NON-BLOCKING eventfd to prevent deadlock risks
                ctx->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
                if (ctx->eventfd < 0)
                    std::terminate();

                ret = io_uring_register_eventfd(&ctx->ring, ctx->eventfd);
                if (ret < 0)
                    std::terminate();

                if (config.pin_threads)
                    pinThreadToCpu(i, i);

                init_latch.count_down();
                init_latch.wait();

                runEventLoop(ctx);
            });
    }
    init_latch.wait();
}

IoUringExecutor::~IoUringExecutor()
{
    if (executor_stopped_)
        return;
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
    // 1. Request stop
    if (!stop_source_.request_stop())
    {
        // Already stopped/stopping? We still ensure wake up.
    }

    // 2. Wake all threads so they exit their loops
    for (auto& ctx : contexts_)
    {
        wakeThread(*ctx);
    }

    // 3. Wait for threads to acknowledge stop
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
        return schedule(std::move(func));
    auto* pctx = static_cast<PerThreadContext*>(ctx);
    return scheduleOn(pctx->context_id, std::move(func));
}

size_t IoUringExecutor::pickContextId()
{
    return selectContext().context_id;
}

// ============================================================================
// FIX 2: ROBUST EVENT LOOP
// ============================================================================
void IoUringExecutor::runEventLoop(PerThreadContext* ctx)
{
    while (!ctx->stop_token.stop_requested())
    {
        bool made_progress = false;

        // 1. Process Local Task Queue
        // Returns true if queue is NOT empty (meaning we hit batch limit or have more work)
        made_progress |= processLocalQueue(ctx, executor_config_.local_batch_size);

        // 2. Process Completions
        unsigned head;
        io_uring_cqe* cqe;
        unsigned count = 0;
        io_uring_for_each_cqe(&ctx->ring, head, cqe)
        {
            handleCompletion(ctx, cqe);
            count++;
        }
        if (count > 0)
        {
            io_uring_cq_advance(&ctx->ring, count);
            made_progress = true;
        }

        // 3. Submit pending SQEs
        // We do this unconditionally to ensure I/O is started
        io_uring_submit(&ctx->ring);

        // 4. Wait ONLY if we did no work and have no pending tasks
        if (!made_progress)
        {
            // Efficient Wait:
            // We use poll on the eventfd to sleep until signaled.
            pollfd pfd;
            pfd.fd = ctx->eventfd;
            pfd.events = POLLIN;

            // Wait indefinitely (-1) until eventfd is signaled or signal/stop token interrupts
            // NOTE: In a real system, you might want a short timeout to check stop_token,
            // but here checking stop_requested() at loop top + stop() waking threads is sufficient.
            int ret = poll(&pfd, 1, -1);

            if (ret > 0)
            {
                // Consume the event
                uint64_t val;
                read(ctx->eventfd, &val, sizeof(val));
            }
        }
        else
        {
            // If we made progress, just peek/consume eventfd to clear it without waiting
            // This prevents "stale" wakeups from triggering a useless loop later
            uint64_t val;
            ssize_t n = read(ctx->eventfd, &val, sizeof(val));
            (void)n;  // Ignore EAGAIN
        }
    }

    ctx->setStopped();
    stop_latch_.count_down();
}

// Returns true if there might be more work (limit reached), false if queue empty
bool IoUringExecutor::processLocalQueue(PerThreadContext* ctx, size_t batch)
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
    // If we processed exactly batch size, assume there might be more work.
    return processed == batch;
}

void IoUringExecutor::wakeThread(PerThreadContext& ctx)
{
    uint64_t val = 1;
    // We ignore result; if buffer full, thread is already awake/busy
    // With EFD_NONBLOCK, this won't block.
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