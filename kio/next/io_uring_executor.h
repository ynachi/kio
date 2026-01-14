//
// io_uring_executor_lockfree.h
// Pure io_uring design with per-thread control rings
//
// Architecture:
// - Each thread has TWO rings:
//   1. Worker ring: processes tasks and I/O operations
//   2. Control ring: sends cross-thread messages to other workers
// - Zero mutex locks in hot path
// - True SINGLE_ISSUER on all rings
// - Optimal for high-performance storage engines and network proxies
//

#ifndef KIO_CORE_URING_EXECUTOR_LOCKFREE_H
#define KIO_CORE_URING_EXECUTOR_LOCKFREE_H

#include <atomic>
#include <coroutine>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#include <async_simple/Executor.h>

namespace kio::next::v1
{

struct IoUringExecutorConfig
{
    size_t num_threads = std::thread::hardware_concurrency();
    uint32_t io_uring_entries = 32768;
    uint32_t io_uring_flags = 0;

    // Each worker has its own control ring for sending messages
    uint32_t control_ring_entries = 1024;  // Smaller than worker ring

    // Batching configuration
    size_t max_cqe_batch = 256;
    size_t task_pool_size = 1024;

    bool pin_threads = false;
    bool enable_metrics = false;
};

// Forward declaration
class IoUringExecutor;

// Base class for all io_uring operations
struct IoOp
{
    virtual void complete(int res) = 0;
    virtual ~IoOp() = default;
};

// Memory pool for task allocations
template <typename T>
class ObjectPool
{
public:
    explicit ObjectPool(size_t initial_size)
    {
        pool_.reserve(initial_size);
        for (size_t i = 0; i < initial_size; ++i)
        {
            pool_.push_back(std::make_unique<T>());
        }
    }

    template <typename... Args>
    T* acquire(Args&&... args)
    {
        std::lock_guard lock(mutex_);

        if (!pool_.empty())
        {
            auto obj = std::move(pool_.back());
            pool_.pop_back();
            T* ptr = obj.release();
            if constexpr (sizeof...(Args) > 0)
            {
                ptr->~T();
                new (ptr) T(std::forward<Args>(args)...);
            }
            return ptr;
        }

        allocated_count_.fetch_add(1, std::memory_order_relaxed);
        return new T(std::forward<Args>(args)...);
    }

    void release(T* obj)
    {
        if (!obj)
            return;

        std::lock_guard lock(mutex_);

        if (pool_.size() < max_pool_size_)
        {
            pool_.push_back(std::unique_ptr<T>(obj));
        }
        else
        {
            delete obj;
        }
    }

    size_t size() const
    {
        std::lock_guard lock(mutex_);
        return pool_.size();
    }

    size_t allocated_count() const { return allocated_count_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<T>> pool_;
    std::atomic<size_t> allocated_count_{0};
    static constexpr size_t max_pool_size_ = 2048;
};

class IoUringExecutor : public async_simple::Executor
{
public:
    using Func = std::function<void()>;
    using Task = Func;

    explicit IoUringExecutor(const IoUringExecutorConfig& config = IoUringExecutorConfig{});
    ~IoUringExecutor() override;

    IoUringExecutor(const IoUringExecutor&) = delete;
    IoUringExecutor& operator=(const IoUringExecutor&) = delete;
    IoUringExecutor(IoUringExecutor&&) = delete;
    IoUringExecutor& operator=(IoUringExecutor&&) = delete;

    // async_simple::Executor interface
    bool schedule(Func func) override;
    [[nodiscard]] bool currentThreadInExecutor() const override;
    [[nodiscard]] size_t currentContextId() const override;
    Context checkout() override;
    bool checkin(Func func, Context ctx, async_simple::ScheduleOptions opts) override;

    // Extended interface
    bool scheduleOn(size_t context_id, Func func);
    bool scheduleLocal(Func func);
    size_t pickContextId();

    void stop();
    void join();

    [[nodiscard]] size_t numThreads() const { return contexts_.size(); }
    [[nodiscard]] io_uring* getRing(size_t context_id) const { return &contexts_[context_id]->ring; }
    [[nodiscard]] std::stop_token getStopToken() const noexcept { return stop_source_.get_token(); }

    // Performance metrics
    struct Metrics
    {
        std::atomic<uint64_t> tasks_scheduled{0};
        std::atomic<uint64_t> tasks_completed{0};
        std::atomic<uint64_t> io_ops_completed{0};
        std::atomic<uint64_t> cross_thread_schedules{0};
        std::atomic<uint64_t> same_thread_schedules{0};
        std::atomic<uint64_t> external_schedules{0};
        std::atomic<uint64_t> batches_processed{0};
    };

    [[nodiscard]] const Metrics& getMetrics() const { return metrics_; }

private:
    struct TaskOp;

    struct PerThreadContext
    {
        // Worker ring: processes tasks and I/O
        io_uring ring{};

        // Control ring: THIS thread uses it to send messages to OTHER threads
        // Only this thread ever submits to this control_ring (true SINGLE_ISSUER)
        io_uring control_ring{};

        std::thread::id thread_id;
        size_t context_id;
        std::atomic<bool> running{true};
        std::stop_token stop_token;
        // Per-thread task pool
        std::unique_ptr<ObjectPool<TaskOp>> task_pool;

        explicit PerThreadContext(size_t id, size_t pool_size)
            : context_id(id), task_pool(std::make_unique<ObjectPool<TaskOp>>(pool_size))
        {
        }

        bool setStopped()
        {
            bool expected = true;
            return running.compare_exchange_strong(expected, false);
        }

        ~PerThreadContext() { setStopped(); }
    };

    // Task wrapper
    struct TaskOp : public IoOp
    {
        Func fn;
        PerThreadContext* owner_ctx{nullptr};

        TaskOp() = default;
        explicit TaskOp(Func&& f) : fn(std::move(f)) {}

        void complete(int) override
        {
            try
            {
                if (fn)
                    fn();
            }
            catch (...)
            {
                // Log or handle exceptions
            }

            if (owner_ctx && owner_ctx->task_pool)
            {
                fn = nullptr;
                owner_ctx->task_pool->release(this);
            }
            else
            {
                delete this;
            }
        }
    };

    static thread_local PerThreadContext* current_context_;

    std::vector<std::unique_ptr<PerThreadContext>> contexts_;
    std::vector<std::jthread> threads_;
    std::stop_source stop_source_;
    std::atomic<bool> executor_stopped_{false};
    std::latch stop_latch_;
    std::atomic<size_t> next_context_{0};
    IoUringExecutorConfig executor_config_;

    // Fallback control ring for external threads (threads not in executor)
    // Protected by mutex since we don't know which external thread will use it
    io_uring external_control_ring_{};
    std::mutex external_control_mutex_;

    // Metrics
    Metrics metrics_;

    void runEventLoop(PerThreadContext* ctx);
    PerThreadContext& selectContext();
    void pinThreadToCpu(size_t thread_id, size_t cpu_id);
    static void handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe, bool& is_task);

    bool submitTask(size_t context_id, Func func);
    bool submitTaskSameThread(PerThreadContext* ctx, Func func);
    bool submitTaskCrossThread(size_t context_id, Func func);
    bool submitTaskExternal(size_t context_id, Func func);
};

enum class ResumeMode : uint8_t
{
    InlineOnSubmitCtx,
    ViaCheckin
};

template <typename ResultType, typename PrepareFunc>
class IoUringAwaiter : public IoOp
{
public:
    using result_type = ResultType;

    IoUringAwaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
        : executor_(executor), prepare_func_(std::move(prepare_func))
    {
    }

    IoUringAwaiter&& on_context(size_t ctx) &&
    {
        forced_ctx_ = true;
        context_id_ = ctx;
        return std::move(*this);
    }

    IoUringAwaiter&& resume_on(async_simple::Executor::Context home, async_simple::ScheduleOptions opts = {}) &&
    {
        resume_mode_ = ResumeMode::ViaCheckin;
        home_ctx_ = home;
        opts_ = opts;
        return std::move(*this);
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        continuation_ = h;

        if (!forced_ctx_)
        {
            context_id_ =
                executor_->currentThreadInExecutor() ? executor_->currentContextId() : executor_->pickContextId();
        }

        if (resume_mode_ == ResumeMode::ViaCheckin && home_ctx_ == IoUringExecutor::NULLCTX)
        {
            home_ctx_ = executor_->checkout();
        }

        if (executor_->currentThreadInExecutor() && executor_->currentContextId() == context_id_)
        {
            return submit_sqe_or_resume_error();
        }

        const bool ok = executor_->scheduleOn(context_id_,
                                              [this]()
                                              {
                                                  if (!this->submit_sqe())
                                                  {
                                                      this->result_ = -EBUSY;
                                                      this->complete(this->result_);
                                                  }
                                              });

        if (!ok)
        {
            result_ = -ECANCELED;
            return false;
        }

        return true;
    }

    ResultType await_resume()
    {
        if (result_ < 0)
        {
            errno = -result_;
            throw std::system_error(errno, std::system_category(), "io_uring operation failed");
        }
        return static_cast<ResultType>(result_);
    }

    void complete(const int res) override
    {
        result_ = res;
        if (!continuation_)
            return;

        if (resume_mode_ == ResumeMode::InlineOnSubmitCtx)
        {
            continuation_.resume();
            return;
        }

        auto h = continuation_;
        continuation_ = {};
        executor_->checkin([h]() mutable { h.resume(); }, home_ctx_, opts_);
    }

private:
    bool submit_sqe_or_resume_error() noexcept
    {
        if (submit_sqe())
            return true;
        result_ = -EBUSY;
        return false;
    }

    bool submit_sqe() noexcept
    {
        io_uring* ring = executor_->getRing(context_id_);
        io_uring_sqe* sqe = io_uring_get_sqe(ring);

        if (!sqe)
        {
            if (const int submitted = io_uring_submit(ring); submitted < 0)
                return false;
            sqe = io_uring_get_sqe(ring);
            if (!sqe)
                return false;
        }

        prepare_func_(sqe);
        io_uring_sqe_set_data(sqe, static_cast<IoOp*>(this));
        return true;
    }

    IoUringExecutor* executor_;
    size_t context_id_{0};
    bool forced_ctx_{false};

    PrepareFunc prepare_func_;
    std::coroutine_handle<> continuation_;
    int result_{0};

    ResumeMode resume_mode_{ResumeMode::InlineOnSubmitCtx};
    async_simple::Executor::Context home_ctx_{IoUringExecutor::NULLCTX};
    async_simple::ScheduleOptions opts_{};
};

template <typename ResultType, typename PrepareFunc>
auto make_io_awaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
{
    return IoUringAwaiter<ResultType, std::decay_t<PrepareFunc>>(executor, std::forward<PrepareFunc>(prepare_func));
}

}  // namespace kio::next::v1

#endif  // KIO_CORE_URING_EXECUTOR_LOCKFREE_H