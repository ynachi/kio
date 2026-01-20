#pragma once
////////////////////////////////////////////////////////////////////////////////
// runtime.h - Multi-threaded io_uring runtime
//
// Thread-per-ring architecture with cross-thread scheduling via eventfd.
// Each thread owns its ring - no contention on the hot path.
// Cross-thread scheduling is explicit and opt-in.
////////////////////////////////////////////////////////////////////////////////

// Fix: Linux kernel headers (via liburing) define a macro BLOCK_SIZE (in linux/fs.h).
// This conflicts with concurrentqueue.h which uses BLOCK_SIZE as a member.
// We undefine it here to prevent the collision.
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

// NOTE: moodycamel::ConcurrentQueue has built-in TSAN support.
// When compiled with -fsanitize=thread, the compiler defines __SANITIZE_THREAD__
// which enables the queue's internal TSAN annotations automatically.
// No manual annotations needed!
#include "external_libraries/concurrentqueue.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <latch>
#include <memory>
#include <semaphore>
#include <thread>
#include <utility>
#include <vector>

#include "kio/executor.hpp"

namespace kio
{

class Runtime;
class ThreadContext;

// ----------------------------------------------------------------------------
// Runtime Configuration (User Facing)
// ----------------------------------------------------------------------------
struct RuntimeConfig
{
    // Threading
    size_t num_threads = std::thread::hardware_concurrency();
    bool pin_threads = false;

    // Executor / io_uring Settings
    unsigned entries = 256;
    unsigned uring_flags = 0;        // e.g. IORING_SETUP_SQPOLL
    unsigned sq_thread_idle_ms = 0;  // Only if SQPOLL is set

    // Blocking pool configuration (used by ThreadContext::spawn_blocking)
    size_t blocking_threads = std::max<size_t>(1, std::thread::hardware_concurrency() / 2);
    size_t blocking_queue = 4096;
};

namespace detail
{
// TLS pointer set by each runtime I/O thread.
inline thread_local ThreadContext* tls_current_ctx = nullptr;

// ----------------------------------------------------------------------------
// BlockingThreadPool - Lock-free Queue + Semaphores
// ----------------------------------------------------------------------------
class BlockingThreadPool
{
public:
    using Job = std::move_only_function<void()>;

    explicit BlockingThreadPool(size_t threads, size_t max_queue = 1);
    ~BlockingThreadPool() noexcept;

    BlockingThreadPool(const BlockingThreadPool&) = delete;
    BlockingThreadPool& operator=(const BlockingThreadPool&) = delete;

    bool try_submit(Job job);
    size_t get_queue_size() const;

private:
    void worker_loop();

    moodycamel::ConcurrentQueue<Job> queue_;

    std::counting_semaphore<> slots_available_;
    std::counting_semaphore<> tasks_available_;

    std::vector<std::thread> workers_;
    size_t max_queue_;
    std::atomic<bool> stop_{false};
};

}  // namespace detail

////////////////////////////////////////////////////////////////////////////////
// ScheduleOnAwaiter - Switch execution to a different thread
////////////////////////////////////////////////////////////////////////////////

class ScheduleOnAwaiter
{
    ThreadContext& target_;

public:
    explicit ScheduleOnAwaiter(ThreadContext& target) : target_(target) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const;
    void await_resume() const noexcept {}
};

////////////////////////////////////////////////////////////////////////////////
// ThreadContext - A single thread with its own io_uring
////////////////////////////////////////////////////////////////////////////////

class ThreadContext
{
    friend class Runtime;
    friend class ScheduleOnAwaiter;

public:
    using WorkItem = std::move_only_function<void()>;

private:
    detail::Executor exec_;
    int eventfd_ = -1;

    Runtime* runtime_ = nullptr;

    moodycamel::ConcurrentQueue<WorkItem> incoming_;
    std::atomic<size_t> pending_work_{0};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};


    detail::BlockingThreadPool* blocking_ = nullptr;

    // To avoid making eventfd write system call when the worker
    // is already active
    std::atomic<bool> active_{false};
    size_t index_;

    static ThreadContext* current() noexcept { return detail::tls_current_ctx; }


    struct EventfdReadOp : detail::BaseOp
    {
        ThreadContext& ctx;
        uint64_t buf = 0;

        explicit EventfdReadOp(ThreadContext& c) : ctx(c) {}

        void await_suspend(std::coroutine_handle<> h)
        {
            handle = h;
            io_uring_sqe* sqe = ctx.exec_.get_sqe();
            io_uring_prep_read(sqe, ctx.eventfd_, &buf, sizeof(buf), 0);
            io_uring_sqe_set_data(sqe, this);
        }

        uint64_t await_resume() { return buf; }
    };

    // Keep one eventfd read pending forever (until stop).
    Task<> park_task()
    {
        while (!stop_requested_.load(std::memory_order_relaxed))
        {
            EventfdReadOp op(*this);
            const auto r = co_await op;
            (void)r;  // just a wake signal
        }
    }

    // Foreign-thread wakeup path.
    void wake_eventfd() const noexcept;

    // Drain a bounded number of queued WorkItems.
    // Returns the number executed.
    size_t drain_incoming(std::vector<WorkItem>& buf, size_t max_items);

    // Internal wakeup path (runtime thread -> runtime thread): MSG_RING.
    void wake_msg_ring_from(ThreadContext& source) const noexcept;

    void run_thread(bool pin, size_t cpu_index);

public:
    explicit ThreadContext(size_t index,
                           RuntimeConfig cfg,
                           detail::BlockingThreadPool* blocking = nullptr);

    // Helper for Runtime to construct with mapped config
    explicit ThreadContext(size_t index,
                           detail::ExecutorConfig cfg,
                           detail::BlockingThreadPool* blocking = nullptr);

    ~ThreadContext() noexcept;

    ThreadContext(const ThreadContext&) = delete;
    ThreadContext& operator=(const ThreadContext&) = delete;

    // --- Accessor to Executor ---
    // Returns reference to detail::Executor.
    // Users should not use the returned type explicitly, but it's necessary for Ops.
    detail::Executor& executor() { return exec_; }
    const detail::Executor& executor() const { return exec_; }

    size_t index() const { return index_; }
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Schedule work on this thread
    void schedule(WorkItem work);
    void schedule(Task<> task);

    // -------------------------------------------------------------------------
    // Spawn Blocking - bounded offload to background pool, resume on this ctx
    // -------------------------------------------------------------------------
    template <typename F>
    auto spawn_blocking(F&& func) -> Task<Result<std::invoke_result_t<std::decay_t<F>>>>
    {
        using ReturnType = std::invoke_result_t<std::decay_t<F>>;

        struct State
        {
            ThreadContext* ctx = nullptr;
            std::coroutine_handle<> h{};
            std::atomic<bool> canceled{false};
            Result<ReturnType> value;
            std::decay_t<F> fn;

            State(ThreadContext* c, std::decay_t<F>&& f) : ctx(c), fn(std::move(f)) {}
        };

        auto state = std::make_shared<State>(this, std::decay_t<F>(std::forward<F>(func)));

        struct BlockingAwaiter
        {
            ThreadContext& ctx;
            detail::BlockingThreadPool* pool;
            std::shared_ptr<State> s;

            ~BlockingAwaiter()
            {
                s->canceled.store(true, std::memory_order_release);
            }

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h)
            {
                s->h = h;

                auto job = [st = s]() mutable
                {
                    try
                    {
                        if constexpr (std::is_void_v<ReturnType>)
                        {
                            st->fn();
                            st->value = {};
                        }
                        else
                        {
                            st->value = st->fn();
                        }
                    }
                    catch (...)
                    {
                        st->value = std::unexpected(std::make_error_code(std::errc::io_error));
                    }

                    if (st->canceled.load(std::memory_order_acquire))
                        return;

                    // Resume back on the owning io thread.
                    st->ctx->schedule([st]()
                    {
                        if (st->canceled.load(std::memory_order_acquire))
                            return;
                        st->h.resume();
                    });
                };

                if (!pool || !pool->try_submit(std::move(job)))
                {
                    // Backpressure: don't block an io thread.
                    s->value = std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
                    ctx.schedule([h]() { h.resume(); });
                }
            }

            Result<ReturnType> await_resume()
            {
                return std::move(s->value);
            }
        };

        co_return co_await BlockingAwaiter{*this, blocking_, std::move(state)};
    }

private:
    void start(bool pin);
    void request_stop();
    void join();
};


////////////////////////////////////////////////////////////////////////////////
// ScheduleOnAwaiter helper
////////////////////////////////////////////////////////////////////////////////

inline ScheduleOnAwaiter schedule_on(ThreadContext& target)
{
    return ScheduleOnAwaiter(target);
}

////////////////////////////////////////////////////////////////////////////////
// Runtime - Manages multiple threads
////////////////////////////////////////////////////////////////////////////////

class Runtime
{
    detail::BlockingThreadPool blocking_pool_;
    std::vector<std::unique_ptr<ThreadContext>> threads_;
    std::atomic<size_t> next_thread_{0};
    bool started_ = false;

public:
    explicit Runtime(RuntimeConfig cfg = RuntimeConfig{});
    ~Runtime();

    size_t size() const { return threads_.size(); }

    ThreadContext& thread(size_t index) { return *threads_.at(index); }
    const ThreadContext& thread(size_t index) const { return *threads_.at(index); }

    ThreadContext& next_thread()
    {
        const size_t idx = next_thread_.fetch_add(1, std::memory_order_relaxed) % threads_.size();
        return *threads_[idx];
    }

    void loop_forever(bool pin_threads = false);
    void stop();

    bool running() const
    {
        return started_ &&
               std::ranges::any_of(threads_.begin(), threads_.end(), [](const auto& ctx) { return ctx->running(); });
    }

    void schedule(Task<> task) { next_thread().schedule(std::move(task)); }
    void schedule(ThreadContext::WorkItem work) { next_thread().schedule(std::move(work)); }
};

////////////////////////////////////////////////////////////////////////////////
// Utility: Run a task on a specific thread and wait for completion
////////////////////////////////////////////////////////////////////////////////

template <typename T>
T block_on(ThreadContext& ctx, Task<T> task)
{
    std::optional<T> result;
    std::exception_ptr exception;
    std::latch latch{1};

    auto wrapper = [&]() -> Task<>
    {
        try
        {
            if constexpr (std::is_void_v<T>)
            {
                co_await std::move(task);
            }
            else
            {
                result = co_await std::move(task);
            }
        }
        catch (...)
        {
            exception = std::current_exception();
        }
        latch.count_down();
    };

    ctx.schedule(wrapper());
    latch.wait();

    if (exception)
        std::rethrow_exception(exception);
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

template <>
inline void block_on<void>(ThreadContext& ctx, Task<> task)
{
    std::exception_ptr exception;
    std::latch latch{1};

    auto wrapper = [&]() -> Task<>
    {
        try
        {
            co_await std::move(task);
        }
        catch (...)
        {
            exception = std::current_exception();
        }
        latch.count_down();
    };

    ctx.schedule(wrapper());
    latch.wait();

    if (exception)
        std::rethrow_exception(exception);
}

}  // namespace uring