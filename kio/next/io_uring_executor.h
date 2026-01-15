#ifndef KIO_CORE_URING_EXECUTOR_H
#define KIO_CORE_URING_EXECUTOR_H

#include <atomic>
#include <coroutine>
#include <cstring>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

#include <async_simple/Executor.h>

namespace kio::next::v1
{

// ============================================================================
// Configuration
// ============================================================================

struct IoUringExecutorConfig
{
    size_t num_threads = std::thread::hardware_concurrency();
    uint32_t io_uring_entries = 32768;
    uint32_t io_uring_flags = 0;
    uint32_t control_ring_entries = 2048;

    size_t max_cqe_batch = 256;
    size_t task_pool_size = 4096;

    bool pin_threads = false;
    bool enable_metrics = false;
    bool immediate_submit = false;
};

// ============================================================================
// RAII Wrapper for io_uring
// ============================================================================

class IoUringRing
{
public:
    IoUringRing() = default;

    ~IoUringRing()
    {
        if (initialized_)
        {
            io_uring_queue_exit(&ring_);
        }
    }

    IoUringRing(const IoUringRing&) = delete;
    IoUringRing& operator=(const IoUringRing&) = delete;

    IoUringRing(IoUringRing&& other) noexcept : ring_(other.ring_), initialized_(other.initialized_)
    {
        other.initialized_ = false;
    }

    IoUringRing& operator=(IoUringRing&& other) noexcept
    {
        if (this != &other)
        {
            if (initialized_)
            {
                io_uring_queue_exit(&ring_);
            }
            ring_ = other.ring_;
            initialized_ = other.initialized_;
            other.initialized_ = false;
        }
        return *this;
    }

    int init(const uint32_t entries, io_uring_params* params)
    {
        if (initialized_)
        {
            return -EBUSY;
        }

        const int ret = io_uring_queue_init_params(entries, &ring_, params);
        if (ret == 0)
        {
            initialized_ = true;
        }
        return ret;
    }

    [[nodiscard]] bool initialized() const { return initialized_; }
    [[nodiscard]] io_uring* get() { return initialized_ ? &ring_ : nullptr; }
    [[nodiscard]] const io_uring* get() const { return initialized_ ? &ring_ : nullptr; }
    [[nodiscard]] int fd() const { return initialized_ ? ring_.ring_fd : -1; }

    io_uring* operator->() { return &ring_; }
    io_uring& operator*() { return ring_; }

private:
    io_uring ring_{};
    bool initialized_ = false;
};

// ============================================================================
// Inline Function - Using C++23 std::move_only_function
// ============================================================================

using InlineFunction = std::move_only_function<void()>;

// ============================================================================
// IoOp Base with Type Tag
// ============================================================================

struct IoOp
{
    enum class Type : uint8_t
    {
        Task,
        Io,
        PoolReturn
    };

    Type type;

    explicit IoOp(Type t) : type(t) {}
    virtual void complete(int res) = 0;
    virtual ~IoOp() = default;
};

class IoUringExecutor;

// ============================================================================
// Simple Object Pool
// ============================================================================

template <typename T>
class ObjectPool
{
public:
    explicit ObjectPool(size_t initial_size)
    {
        free_list_.reserve(initial_size);
        for (size_t i = 0; i < initial_size; ++i)
        {
            free_list_.push_back(new T());
        }
    }

    ~ObjectPool() { shutdown(); }

    void shutdown()
    {
        for (T* p : free_list_)
        {
            delete p;
        }
        free_list_.clear();
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* acquire(Args&&... args)
    {
        if (!free_list_.empty())
        {
            T* obj = free_list_.back();
            free_list_.pop_back();

            // Reinitialize in place
            obj->~T();
            new (obj) T(std::forward<Args>(args)...);
            return obj;
        }

        return new T(std::forward<Args>(args)...);
    }

    void release(T* obj)
    {
        if (obj == nullptr)
        {
            return;
        }

        if (free_list_.size() < max_pool_size_)
        {
            free_list_.push_back(obj);
        }
        else
        {
            delete obj;
        }
    }

private:
    std::vector<T*> free_list_;
    static constexpr size_t max_pool_size_ = 32768;
};

// ============================================================================
// IoUringExecutor
// ============================================================================

class IoUringExecutor : public async_simple::Executor
{
public:
    using InternalFunc = InlineFunction;

    explicit IoUringExecutor(const IoUringExecutorConfig& config = IoUringExecutorConfig{});
    ~IoUringExecutor() override;

    IoUringExecutor(const IoUringExecutor&) = delete;
    IoUringExecutor& operator=(const IoUringExecutor&) = delete;
    IoUringExecutor(IoUringExecutor&&) = delete;
    IoUringExecutor& operator=(IoUringExecutor&&) = delete;

    bool schedule(Func func) override;
    [[nodiscard]] bool currentThreadInExecutor() const override;
    [[nodiscard]] size_t currentContextId() const override;
    Context checkout() override;
    bool checkin(Func func, Context ctx, async_simple::ScheduleOptions opts) override;

    bool scheduleOn(size_t context_id, InternalFunc func);
    bool scheduleLocal(InternalFunc func);
    size_t pickContextId();

    void stop();
    void join();

    [[nodiscard]] size_t numThreads() const { return contexts_.size(); }
    [[nodiscard]] io_uring* getRing(size_t context_id) const { return contexts_[context_id]->ring.get(); }
    [[nodiscard]] std::stop_token getStopToken() const noexcept { return stop_source_.get_token(); }
    [[nodiscard]] bool immediateSubmit() const noexcept { return executor_config_.immediate_submit; }

    struct Metrics
    {
        std::atomic<uint64_t> tasks_scheduled{0};
        std::atomic<uint64_t> tasks_completed{0};
        std::atomic<uint64_t> io_ops_completed{0};
        std::atomic<uint64_t> cross_thread_schedules{0};
        std::atomic<uint64_t> same_thread_schedules{0};
        std::atomic<uint64_t> external_schedules{0};
        std::atomic<uint64_t> batches_processed{0};
        std::atomic<uint64_t> pool_returns_via_ring{0};
    };

    [[nodiscard]] const Metrics& getMetrics() const { return metrics_; }

private:
    struct TaskOp;

    // Cache-aligned to prevent false sharing between threads
    struct alignas(64) PerThreadContext
    {
        IoUringRing ring;
        IoUringRing control_ring;

        std::thread::id thread_id;
        size_t context_id;
        std::atomic<bool> running{true};
        std::stop_token stop_token;

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

    struct TaskOp : IoOp
    {
        InternalFunc fn;
        PerThreadContext* owner_ctx{nullptr};
        PerThreadContext* source_ctx{nullptr};

        TaskOp() : IoOp(Type::Task) {}
        explicit TaskOp(InternalFunc&& f) : IoOp(Type::Task), fn(std::move(f)) {}

        void complete(int) override
        {
            if (fn)
            {
                fn();
            }
            fn = nullptr;
            returnToPool();
        }

        void reclaim()
        {
            if (source_ctx != nullptr && source_ctx->task_pool)
            {
                source_ctx->task_pool->release(this);
            }
            else
            {
                delete this;
            }
        }

    private:
        void returnToPool();
    };

    static thread_local PerThreadContext* current_context_;

    std::vector<std::unique_ptr<PerThreadContext>> contexts_;
    std::vector<std::jthread> threads_;
    std::stop_source stop_source_;
    std::atomic<bool> executor_stopped_{false};
    std::latch stop_latch_;
    std::atomic<size_t> next_context_{0};
    IoUringExecutorConfig executor_config_;

    IoUringRing external_control_ring_;
    std::mutex external_control_mutex_;

    Metrics metrics_;

    void runEventLoop(PerThreadContext* ctx);
    PerThreadContext& selectContext();
    void pinThreadToCpu(size_t thread_id, size_t cpu_id);
    void handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe, bool& is_task);

    bool submitTask(size_t context_id, InternalFunc func);
    bool submitTaskSameThread(PerThreadContext* ctx, InternalFunc func);
    bool submitTaskCrossThread(size_t context_id, InternalFunc func);
    bool submitTaskExternal(size_t context_id, InternalFunc func);
    void validateConfig();
    void initExternalControlRing();
    void workerThreadEntry(size_t index, std::shared_ptr<std::latch> init_latch, std::atomic<bool>& init_failed,
                           std::atomic<int>& init_error);
};

// ============================================================================
// Resume Mode
// ============================================================================

enum class ResumeMode : uint8_t
{
    InlineOnSubmitCtx,
    ViaCheckin
};

// ============================================================================
// IoUringAwaiter
// ============================================================================

template <typename ResultType, typename PrepareFunc>
class IoUringAwaiter : public IoOp
{
public:
    using result_type = ResultType;

    IoUringAwaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
        : IoOp(Type::Io), executor_(executor), prepare_func_(std::move(prepare_func))
    {
    }

    IoUringAwaiter(IoUringAwaiter&& other) noexcept
        : IoOp(Type::Io),
          executor_(other.executor_),
          context_id_(other.context_id_),
          forced_ctx_(other.forced_ctx_),
          prepare_func_(std::move(other.prepare_func_)),
          continuation_(other.continuation_),
          result_(other.result_),
          resume_mode_(other.resume_mode_),
          home_ctx_(other.home_ctx_),
          opts_(other.opts_)
    {
        other.continuation_ = nullptr;
    }

    IoUringAwaiter& operator=(IoUringAwaiter&&) = delete;
    IoUringAwaiter(const IoUringAwaiter&) = delete;
    IoUringAwaiter& operator=(const IoUringAwaiter&) = delete;

    [[nodiscard]] IoUringAwaiter on_context(const size_t ctx) &&
    {
        forced_ctx_ = true;
        context_id_ = ctx;
        return std::move(*this);
    }

    [[nodiscard]] IoUringAwaiter resume_on(async_simple::Executor::Context home,
                                           async_simple::ScheduleOptions opts = {}) &&
    {
        resume_mode_ = ResumeMode::ViaCheckin;
        home_ctx_ = home;
        opts_ = opts;
        return std::move(*this);
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept
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

        // Optimized same-thread submission
        if (executor_->currentThreadInExecutor() && executor_->currentContextId() == context_id_)
        {
            if (!submit_sqe_inline())
            {
                // Submission failed (ring full), trampoline error to resume
                executor_->scheduleLocal([h]() { h.resume(); });
            }
            return;
        }

        // Cross-thread submission
        IoUringAwaiter* self = this;
        const bool scheduled = executor_->scheduleOn(context_id_,
                                                     [self]()
                                                     {
                                                         io_uring* ring = self->executor_->getRing(self->context_id_);
                                                         io_uring_sqe* sqe = io_uring_get_sqe(ring);

                                                         if (!sqe)
                                                         {
                                                             io_uring_submit(ring);
                                                             sqe = io_uring_get_sqe(ring);
                                                         }

                                                         if (!sqe)
                                                         {
                                                             self->complete(-EBUSY);
                                                             return;
                                                         }

                                                         self->prepare_func_(sqe);
                                                         io_uring_sqe_set_data(sqe, static_cast<IoOp*>(self));

                                                         if (self->executor_->immediateSubmit())
                                                         {
                                                             io_uring_submit(ring);
                                                         }
                                                     });

        if (!scheduled)
        {
            // Schedule failed, trampoline error
            result_ = -ECANCELED;
            if (executor_->currentThreadInExecutor())
            {
                executor_->scheduleLocal([h]() { h.resume(); });
            }
            else
            {
                // Fallback for external thread scheduling failure
                h.resume();
            }
        }
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
        {
            return;
        }

        auto h = continuation_;
        continuation_ = {};

        if (resume_mode_ == ResumeMode::InlineOnSubmitCtx)
        {
            h.resume();
        }
        else
        {
            executor_->checkin([h]() mutable { h.resume(); }, home_ctx_, opts_);
        }
    }

private:
    bool submit_sqe_inline() noexcept
    {
        io_uring* ring = executor_->getRing(context_id_);
        io_uring_sqe* sqe = io_uring_get_sqe(ring);

        if (sqe == nullptr)
        {
            if (io_uring_submit(ring) < 0)
            {
                result_ = -EBUSY;
                return false;
            }
            sqe = io_uring_get_sqe(ring);
            if (sqe == nullptr)
            {
                result_ = -EBUSY;
                return false;
            }
        }

        prepare_func_(sqe);
        io_uring_sqe_set_data(sqe, static_cast<IoOp*>(this));

        if (executor_->immediateSubmit())
        {
            io_uring_submit(ring);
        }

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
    async_simple::ScheduleOptions opts_;
};

template <typename ResultType, typename PrepareFunc>
auto make_io_awaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
{
    return IoUringAwaiter<ResultType, std::decay_t<PrepareFunc>>(executor, std::forward<PrepareFunc>(prepare_func));
}

}  // namespace kio::next::v1

#endif  // KIO_CORE_URING_EXECUTOR_H