#pragma once
////////////////////////////////////////////////////////////////////////////////
// runtime.h - Multi-threaded io_uring runtime
//
// Thread-per-ring architecture with cross-thread scheduling via eventfd.
// Each thread owns its ring - no contention on the hot path.
// Cross-thread scheduling is explicit and opt-in.
////////////////////////////////////////////////////////////////////////////////
#include "external_libraries/concurrentqueue.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <latch>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <sys/eventfd.h>

#include "executor.hpp"

namespace uring
{

class Runtime;
class ThreadContext;

namespace detail
{
// TLS pointer set by each runtime I/O thread.
inline thread_local ThreadContext* tls_current_ctx = nullptr;

// ----------------------------------------------------------------------------
// BlockingThreadPool - bounded queue, fixed worker count
// ----------------------------------------------------------------------------
class BlockingThreadPool
{
public:
    using Job = std::move_only_function<void()>;

    BlockingThreadPool(size_t threads, size_t max_queue)
        : max_queue_(max_queue ? max_queue : 1)
    {
        if (threads == 0)
            threads = 1;
        workers_.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
        {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~BlockingThreadPool()
    {
        {
            std::scoped_lock lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable())
                t.join();
    }

    BlockingThreadPool(const BlockingThreadPool&) = delete;
    BlockingThreadPool& operator=(const BlockingThreadPool&) = delete;

    bool try_submit(Job job)
    {
        std::scoped_lock lk(mu_);
        if (stop_)
            return false;
        if (q_.size() >= max_queue_)
            return false;
        q_.push_back(std::move(job));
        cv_.notify_one();
        return true;
    }

private:
    void worker_loop()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty())
                    return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    std::vector<std::thread> workers_;
    size_t max_queue_ = 1;
    bool stop_ = false;
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
    Executor exec_;
    int eventfd_ = -1;

    Runtime* runtime_ = nullptr;

    moodycamel::ConcurrentQueue<WorkItem> incoming_;
    std::atomic<size_t> pending_work_{0};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Blocking work offload
    detail::BlockingThreadPool* blocking_ = nullptr;

    // To avoid making eventfd write system call when the worker
    // is already active
    std::atomic<bool> active_{false};

    size_t index_;

    static ThreadContext* current() noexcept { return detail::tls_current_ctx; }

    // Eventfd read operation for thread wakeup
    struct EventfdReadOp : BaseOp
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
    void wake_eventfd() const noexcept
    {
        constexpr uint64_t val = 1;
        for (;;)
        {
            const ssize_t n = ::write(eventfd_, &val, sizeof(val));
            if (std::cmp_equal(n ,sizeof(val)))
                return;
            if (n < 0 && errno == EINTR)
                continue;
            // Saturation (EAGAIN) or other errors: best-effort wake.
            return;
        }
    }

    // Drain a bounded number of queued WorkItems.
    // Returns the number executed.
    size_t drain_incoming(std::vector<WorkItem>& buf, const size_t max_items)
    {
        size_t total = 0;

        while (total < max_items)
        {
            buf.clear();
            const size_t want = std::min<size_t>(64, max_items - total);

            // ConcurrentQueue bulk-dequeue into vector
            incoming_.try_dequeue_bulk(std::back_inserter(buf), want);
            if (buf.empty())
                break;

            for (auto& w : buf)
            {
                try
                {
                    w();
                }
                catch (...)
                {
                    // Runtime-level scheduled callbacks should not crash the loop.
                }
            }

            total += buf.size();
            pending_work_.fetch_sub(buf.size(), std::memory_order_acq_rel);
        }

        return total;
    }

    // Internal wakeup path (runtime thread -> runtime thread): MSG_RING.
    void wake_msg_ring_from(ThreadContext& source) const noexcept
    {
        source.exec_.msg_ring_wake(exec_.ring_fd(), 1, 0);
    }

    void run_thread(const bool pin, const size_t cpu_index)
    {
        detail::tls_current_ctx = this;

        if (pin)
        {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_index % std::thread::hardware_concurrency(), &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        }

        running_.store(true, std::memory_order_release);

        // Ensure we always have an eventfd read pending (for a foreign-thread wake).
        exec_.spawn(park_task());

        std::vector<WorkItem> work_buf;
        work_buf.reserve(64);

        while (!stop_requested_.load(std::memory_order_relaxed) ||
               exec_.pending() > 0 ||
               pending_work_.load(std::memory_order_acquire) > 0)
        {
            const bool have_work = pending_work_.load(std::memory_order_acquire) > 0;

            // If we have queued work, don't block waiting for I/O; poll completions and run work.
            // If we don't have work, it's fine to block (park op + msg_ring will wake us).
            (void)exec_.loop_once(!have_work);

            // Run queued work. If there's lots of it, this will loop without blocking
            // because have_work will stay true.
            (void)drain_incoming(work_buf, 1024);

            // If totally idle and not stopping, yield a touch to avoid pathological spins
            // in unusual cases.
            if (!have_work && exec_.pending() == 0 && !stop_requested_.load(std::memory_order_relaxed))
            {
                std::this_thread::yield();
            }
        }

        running_.store(false, std::memory_order_release);
        detail::tls_current_ctx = nullptr;
    }

public:
    explicit ThreadContext(const size_t index,
                           const ExecutorConfig cfg = ExecutorConfig{},
                           detail::BlockingThreadPool* blocking = nullptr)
        : exec_(cfg),
          eventfd_(eventfd(0, EFD_CLOEXEC)),
          blocking_(blocking),
          index_(index)
    {
        if (eventfd_ < 0)
            throw std::system_error(errno, std::system_category(), "eventfd");
    }

    ~ThreadContext()
    {
        if (eventfd_ >= 0)
            ::close(eventfd_);
    }

    ThreadContext(const ThreadContext&) = delete;
    ThreadContext& operator=(const ThreadContext&) = delete;

    Executor& executor() { return exec_; }
    const Executor& executor() const { return exec_; }
    size_t index() const { return index_; }
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Schedule work on this thread
    void schedule(WorkItem work)
    {
        // Increment first so the consumer sees "work exists" before any wake decisions.
        const size_t prev = pending_work_.fetch_add(1, std::memory_order_release);
        incoming_.enqueue(std::move(work));

        // Only wake on transition 0 -> 1.
        if (prev != 0)
            return;

        if (ThreadContext* src = current(); src && src->runtime_ == runtime_)
        {
            if (src != this)
                wake_msg_ring_from(*src);
            else
            {
                // Same-thread schedule: no wake needed; the loop will see pending_work_.
                // (This avoids pointless self-msg-ring traffic.)
            }
        }
        else
        {
            // Foreign thread: use eventfd fallback (park op will complete and wake the ring).
            wake_eventfd();
        }
    }

    void schedule(Task<> task)
    {
        auto handle = task.release();
        schedule([handle]() { handle.resume(); });
    }

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
    void start(bool pin)
    {
        thread_ = std::thread([this, pin]() { run_thread(pin, index_); });
    }

    void request_stop()
    {
        stop_requested_.store(true, std::memory_order_release);
        // request_stop is typically called from a foreign thread; eventfd is the safe wake.
        wake_eventfd();
    }

    void join()
    {
        if (thread_.joinable())
            thread_.join();
    }
};


////////////////////////////////////////////////////////////////////////////////
// ScheduleOnAwaiter implementation
////////////////////////////////////////////////////////////////////////////////

inline void ScheduleOnAwaiter::await_suspend(std::coroutine_handle<> h) const
{
    target_.schedule([h]() { h.resume(); });
}

inline ScheduleOnAwaiter schedule_on(ThreadContext& target)
{
    return ScheduleOnAwaiter(target);
}

////////////////////////////////////////////////////////////////////////////////
// Runtime - Manages multiple threads
////////////////////////////////////////////////////////////////////////////////

struct RuntimeConfig
{
    size_t num_threads = std::thread::hardware_concurrency();
    bool pin_threads = false;
    ExecutorConfig executor_config = {};

    // Blocking pool configuration (used by ThreadContext::spawn_blocking)
    size_t blocking_threads = std::max<size_t>(1, std::thread::hardware_concurrency() / 2);
    size_t blocking_queue = 4096;
};

class Runtime
{
    detail::BlockingThreadPool blocking_pool_;
    std::vector<std::unique_ptr<ThreadContext>> threads_;
    std::atomic<size_t> next_thread_{0};
    bool started_ = false;

public:
    explicit Runtime(RuntimeConfig cfg = RuntimeConfig{})
        : blocking_pool_(cfg.blocking_threads, cfg.blocking_queue)
    {
        if (cfg.num_threads == 0)
            cfg.num_threads = 1;

        threads_.reserve(cfg.num_threads);
        for (size_t i = 0; i < cfg.num_threads; ++i)
        {
            auto ctx = std::make_unique<ThreadContext>(i, cfg.executor_config, &blocking_pool_);
            ctx->runtime_ = this;
            threads_.push_back(std::move(ctx));
        }
    }

    ~Runtime() { stop(); }

    size_t size() const { return threads_.size(); }

    ThreadContext& thread(const size_t index) { return *threads_.at(index); }
    const ThreadContext& thread(const size_t index) const { return *threads_.at(index); }

    ThreadContext& next_thread()
    {
        const size_t idx = next_thread_.fetch_add(1, std::memory_order_relaxed) % threads_.size();
        return *threads_[idx];
    }

    void loop_forever(const bool pin_threads = false)
    {
        if (started_)
            return;
        started_ = true;

        for (const auto& ctx : threads_)
            ctx->start(pin_threads);

        for (const auto& ctx : threads_)
        {
            while (!ctx->running())
                std::this_thread::yield();
        }
    }

    void stop()
    {
        if (!started_)
            return;

        for (const auto& ctx : threads_)
            ctx->request_stop();
        for (const auto& ctx : threads_)
            ctx->join();

        started_ = false;
    }

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