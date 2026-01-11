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
#include <async_simple/IOExecutor.h>

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
     * Must be used with SINGLE_ISSUER.
     * - IORING_SETUP_COOP_TASKRUN: Optimizes task running cooperation.
     * * Legacy/Latency tuning:
     * - IORING_SETUP_SQPOLL: Kernel thread polling (burns CPU, lowest latency).
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

    // Extended interface for better control
    bool scheduleOn(size_t context_id, Func&& func);
    bool scheduleLocal(Func&& func);  // Schedule on the current thread if in executor

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
    void processLocalQueue(PerThreadContext* ctx);
    void wakeThread(PerThreadContext& ctx);
    PerThreadContext& selectContext();
    void pinThreadToCpu(size_t thread_id, size_t cpu_id);

    // Completion handler
    static void handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe);
};

/**
 * @brief Base interface for I/O operations that can be completed
 * Used to avoid std::function allocations in awaiters
 */
struct IoOp
{
    virtual void complete(int res) = 0;
    virtual ~IoOp() = default;
};

/**
 * @brief Base class for io_uring awaiters
 *
 * Handles the coroutine suspension/resumption mechanics without heap allocations.
 * The Awaiter itself is the IoOp that gets passed to io_uring user_data.
 */
template <typename ResultType, typename PrepareFunc>
class IoUringAwaiter : public IoOp
{
public:
    using result_type = ResultType;

    IoUringAwaiter(IoUringExecutor* executor, size_t context_id, PrepareFunc&& prepare_func)
        : executor_(executor), context_id_(context_id), prepare_func_(std::move(prepare_func))
    {
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept
    {
        continuation_ = h;

        io_uring* ring = executor_->getRing(context_id_);
        io_uring_sqe* sqe = io_uring_get_sqe(ring);

        if (sqe)
        {
            // Invoke the lambda to prepare the SQE
            prepare_func_(sqe);

            // Set 'this' as user_data so handleCompletion can call complete()
            io_uring_sqe_set_data(sqe, static_cast<IoOp*>(this));
            io_uring_submit(ring);
            submitted_ = true;
        }
        else
        {
            // Ring is full.
            submitted_ = false;
        }
    }

    ResultType await_resume()
    {
        if (!submitted_)
        {
            throw std::runtime_error("Failed to submit io_uring operation (Ring full?)");
        }
        if (result_ < 0)
        {
            errno = -result_;
            throw std::system_error(errno, std::system_category(), "io_uring operation failed");
        }
        return static_cast<ResultType>(result_);
    }

    // IoOp interface implementation
    void complete(int res) override
    {
        result_ = res;
        // Optimization: Direct resumption on the executor thread.
        if (continuation_)
        {
            continuation_.resume();
        }
    }

private:
    IoUringExecutor* executor_;
    size_t context_id_;
    PrepareFunc prepare_func_;
    std::coroutine_handle<> continuation_;
    int result_ = 0;
    bool submitted_ = false;
};

/**
 * @brief Helper to create a unified awaiter with type deduction
 * Also automatically selects the correct context ID.
 */
template <typename ResultType, typename PrepareFunc>
auto make_io_awaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
{
    // Use the current context if in executor, otherwise pick default (0)
    size_t ctx_id = executor->currentThreadInExecutor() ? executor->currentContextId() : 0;

    // We use std::decay_t to store the lambda type directly in the class
    return IoUringAwaiter<ResultType, std::decay_t<PrepareFunc>>(executor, ctx_id,
                                                                 std::forward<PrepareFunc>(prepare_func));
}
}  // namespace kio::next
#endif  // KIO_CORE_URING_EXECUTOR_H
