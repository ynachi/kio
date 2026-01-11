//
// Created by Yao ACHI on 11/01/2026.
//

#ifndef KIO_CORE_URING_EXECUTOR_H
#define KIO_CORE_URING_EXECUTOR_H

#include "kio/sync/mpsc_queue.h"

#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include <liburing.h>
#include <unistd.h>

#include <sys/eventfd.h>

#include <async_simple/Executor.h>

namespace kio::next
{

struct IoUringExecutorConfig
{
    size_t num_threads = std::thread::hardware_concurrency();
    uint32_t io_uring_entries = 256;
    /**
     * @brief io_uring setup flags
     * * Recommended flags for high performance (Kernel 6.0+):
     * - IORING_SETUP_SINGLE_ISSUER: Critical for Thread-per-Core performance.
     * Removes internal ring locks since only one thread accesses the ring.
     * - IORING_SETUP_DEFER_TASKRUN: Batches completion processing to reduce syscalls.
     * * Recommended: IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN
     * Must be used with SINGLE_ISSUER.
     * - IORING_SETUP_COOP_TASKRUN: Optimizes task running cooperation.
     * * Legacy/Latency tuning:
     * - IORING_SETUP_SQPOLL: Kernel thread polling (burns CPU, lowest latency).
     * We can set multiple flags this way
     * config.io_uring_flags = IORING_SETUP_SINGLE_ISSUER |
                                IORING_SETUP_DEFER_TASKRUN |
                                IORING_SETUP_COOP_TASKRUN;
     */
    uint32_t io_uring_flags = 0;  // e.g., IORING_SETUP_SQPOLL, IORING_SETUP_IOPOLL
    size_t task_queue_size = 1024;
    /**
     * @brief Pin threads to CPU cores
     * Set to true to prevent OS from migrating threads.
     * * CRITICAL: If you see "Half R / Half S" thread states (e.g. 4R + 4S on 8 cores),
     * it means the OS is avoiding Hyperthreads. Enable this to force
     * distribution across all logical cores.
     */
    bool pin_threads = false;  // Pin threads to specific CPUs
};

/**
 * @brief Thread-per-core io_uring-based executor for async_simple
 *
 * Features:
 * - One io_uring instance per thread (no ring contention)
 * - Lock-free MPSC queues for task scheduling
 * - Eventfd-based thread wake-up
 * - Supports all async_simple combinators (collectAll, collectAny, etc.)
 * - NUMA-aware thread placement (optional)
 * - Zero-allocation awaiters (using Awaiter address stability)
 */
class IoUringExecutor : public async_simple::Executor
{
    using Task = std::function<void()>;

public:
    explicit IoUringExecutor(const IoUringExecutorConfig& config = IoUringExecutorConfig{});
    ~IoUringExecutor() override;

    // Disable copy and move
    IoUringExecutor(const IoUringExecutor&) = delete;
    IoUringExecutor& operator=(const IoUringExecutor&) = delete;
    IoUringExecutor(IoUringExecutor&&) = delete;
    IoUringExecutor& operator=(IoUringExecutor&&) = delete;

    // async_simple::Executor interface
    bool schedule(Func func) override;
    [[nodiscard]] bool currentThreadInExecutor() const override;
    [[nodiscard]] size_t currentContextId() const override;

    // async_simple context affinity hooks
    Context checkout() override;
    bool checkin(Func func, Context ctx, async_simple::ScheduleOptions opts) override;

    // Extended interface for better control
    bool scheduleOn(size_t context_id, Func&& func);
    bool scheduleLocal(Func&& func);

    // Pick a context for work submission.
    // If called from an executor thread, returns the current context id.
    // Otherwise, returns a round-robin selected context id.
    size_t pickContextId();

    // Graceful shutdown
    void stop();
    void join();

    [[nodiscard]] size_t numThreads() const { return contexts_.size(); }
    [[nodiscard]] io_uring* getRing(const size_t context_id) const { return &contexts_[context_id]->ring; }

private:
    struct PerThreadContext
    {
        io_uring ring{};
        MPSCQueue<Task> task_queue;
        int eventfd;
        std::thread::id thread_id;
        size_t context_id;
        std::atomic<bool> running{true};
        // For round-robin selection
        std::atomic<size_t> task_count{0};

        explicit PerThreadContext(const size_t id, const size_t queue_size)
            : task_queue(queue_size), eventfd(-1), context_id(id)
        {
        }

        ~PerThreadContext()
        {
            if (eventfd >= 0)
            {
                close(eventfd);
            }
        }
    };

    // Thread-local pointer to the current context
    static thread_local PerThreadContext* current_context_;

    std::vector<std::unique_ptr<PerThreadContext>> contexts_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopped_{false};
    std::atomic<size_t> next_context_{0};  // For round-robin scheduling

    void runEventLoop(PerThreadContext* ctx);
    bool processLocalQueue(PerThreadContext* ctx);
    void wakeThread(PerThreadContext& ctx);
    PerThreadContext& selectContext();
    void pinThreadToCpu(size_t thread_id, size_t cpu_id);

    // Completion handler
    static void handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe);
};

/**
 * @brief Tiny completion record stored inside awaiters.
 *
 * This avoids heap allocations on the hot path.
 * user_data points to this object (which is embedded in the awaiter that lives
 * in the coroutine frame while suspended).
 */
struct IoCompletion
{
    void* self = nullptr;
    void (*complete)(void* self, int res) noexcept = nullptr;
};

/**
 * @brief Robust Base class for io_uring awaiters.
 *
 * Implements a thread-safe state machine to handle races between
 * completion and suspension, and marshals submissions to the correct thread.
 */
template <typename ResultType>
class IoUringAwaiterBase
{
public:
    using result_type = ResultType;

    explicit IoUringAwaiterBase(IoUringExecutor* executor, size_t context_id)
        : executor_(executor), context_id_(context_id)
    {
        // Set up the thunk
        completion_.self = this;
        completion_.complete = &IoUringAwaiterBase::completeThunk;
    }

    // Disable copy
    IoUringAwaiterBase(const IoUringAwaiterBase&) = delete;
    IoUringAwaiterBase& operator=(const IoUringAwaiterBase&) = delete;

    // Enable move
    IoUringAwaiterBase(IoUringAwaiterBase&& other) noexcept
        : executor_(other.executor_),
          context_id_(other.context_id_),
          continuation_(other.continuation_),
          result_(other.result_),
          completion_(other.completion_)
    {
        // We can safely load relaxed here because we assume the awaiter        // being moved hasn't been awaited yet
        // (single thread context before suspend).
        state_.store(other.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        submitted_.store(other.submitted_.load(std::memory_order_relaxed), std::memory_order_relaxed);

        // Update the self pointer in the completion thunk to point to THIS new object
        completion_.self = this;

        // Reset other to safe state (though usually not reused)
        other.completion_.self = nullptr;
    }

    IoUringAwaiterBase& operator=(IoUringAwaiterBase&&) = delete;  // Move assignment rarely needed for awaiters

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        continuation_ = h;
        state_.store(kInit, std::memory_order_relaxed);
        submitted_.store(false, std::memory_order_relaxed);

        // 1. Fast Path: We are on the correct thread.
        if (executor_->currentThreadInExecutor() && executor_->currentContextId() == context_id_)
        {
            if (!submitAndMaybeFail())
            {
                return false;  // Resume immediately (don't suspend)
            }
            return tryMarkSuspendedOrComplete();
        }

        // 2. Slow Path: Cross-thread submission.
        // We must marshal the submission to the owner thread to be thread-safe.
        bool scheduled = executor_->scheduleOn(context_id_,
                                               [this]
                                               {
                                                   if (!this->submitAndMaybeFail())
                                                   {
                                                       return;
                                                   }
                                               });

        if (!scheduled)
        {
            failNoCqe(-EAGAIN);
            return false;
        }

        return tryMarkSuspendedOrComplete();
    }

    ResultType await_resume()
    {
        if (!submitted_.load(std::memory_order_acquire))
        {
            throw std::runtime_error("Failed to submit io_uring operation");
        }
        if (result_ < 0)
        {
            errno = -result_;
            throw std::system_error(errno, std::system_category(), "io_uring operation failed");
        }
        return static_cast<ResultType>(result_);
    }

protected:
    // Helper for derived classes to submit SQEs
    template <class PrepFn>
    bool submitOp(IoCompletion* completion, PrepFn&& prep)
    {
        io_uring* ring = executor_->getRing(context_id_);
        io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe)
            return false;

        prep(sqe);
        io_uring_sqe_set_data(sqe, completion);
        io_uring_submit(ring);
        return true;
    }

    // Pure virtual to be implemented by derived/templated class
    virtual bool submitToRing(IoCompletion* completion) = 0;

    IoUringExecutor* executor_;
    size_t context_id_;
    std::coroutine_handle<> continuation_;
    int result_ = 0;

private:
    static constexpr int kInit = 0;
    static constexpr int kSuspended = 1;
    static constexpr int kCompleted = 2;

    static void completeThunk(void* self, int res) noexcept { static_cast<IoUringAwaiterBase*>(self)->onComplete(res); }

    void onComplete(int res) noexcept
    {
        result_ = res;
        int prev = state_.exchange(kCompleted, std::memory_order_acq_rel);
        // Only resume if the coroutine has already suspended
        if (prev == kSuspended)
        {
            continuation_.resume();
        }
    }

    void failNoCqe(int err) noexcept
    {
        submitted_.store(false, std::memory_order_release);
        result_ = err;
        int prev = state_.exchange(kCompleted, std::memory_order_acq_rel);
        if (prev == kSuspended)
        {
            continuation_.resume();
        }
    }

    bool submitAndMaybeFail() noexcept
    {
        const bool ok = submitToRing(&completion_);
        submitted_.store(ok, std::memory_order_release);
        if (!ok)
        {
            failNoCqe(-EAGAIN);
            return false;
        }
        return true;
    }

    bool tryMarkSuspendedOrComplete() noexcept
    {
        int expected = kInit;
        if (state_.compare_exchange_strong(expected, kSuspended, std::memory_order_release, std::memory_order_acquire))
        {
            return true;  // suspend
        }
        return false;  // already completed
    }

    IoCompletion completion_{};
    std::atomic<int> state_{kInit};
    std::atomic<bool> submitted_{false};
};

/**
 * @brief Generic Lambda-based Awaiter
 * * Combines the robustness of IoUringAwaiterBase with the convenience of lambdas.
 */
template <typename ResultType, typename PrepareFunc>
class IoUringAwaiter : public IoUringAwaiterBase<ResultType>
{
public:
    IoUringAwaiter(IoUringExecutor* executor, size_t context_id, PrepareFunc&& prepare_func)
        : IoUringAwaiterBase<ResultType>(executor, context_id), prepare_func_(std::move(prepare_func))
    {
    }

protected:
    bool submitToRing(IoCompletion* completion) override
    {
        return this->submitOp(completion, [&](io_uring_sqe* sqe) { prepare_func_(sqe); });
    }

private:
    PrepareFunc prepare_func_;
};

/**
 * @brief Helper to create a unified awaiter with type deduction
 */
template <typename ResultType, typename PrepareFunc>
auto make_io_awaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
{
    // If on an executor thread, use that context. Otherwise round-robin.
    size_t ctx_id = executor->currentThreadInExecutor() ? executor->currentContextId() : executor->pickContextId();

    return IoUringAwaiter<ResultType, std::decay_t<PrepareFunc>>(executor, ctx_id,
                                                                 std::forward<PrepareFunc>(prepare_func));
}
}  // namespace kio::next
#endif  // KIO_CORE_URING_EXECUTOR_H
