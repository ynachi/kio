#pragma once
////////////////////////////////////////////////////////////////////////////////
// runtime.h - Multi-threaded io_uring runtime
//
// Thread-per-ring architecture with cross-thread scheduling via eventfd.
// Each thread owns its ring - no contention on the hot path.
// Cross-thread scheduling is explicit and opt-in.
////////////////////////////////////////////////////////////////////////////////
#include "third_party/concurrentqueue.h"

#include <atomic>
#include <functional>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
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
    moodycamel::ConcurrentQueue<WorkItem> incoming_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // To avoid making eventfd write system call when the worker
    // is already active
    std::atomic<bool> active_{false};

    size_t index_;

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

    // Internal task to handle incoming cross-thread work
    Task<> eventfd_listener()
    {
        std::vector<WorkItem> items;
        items.reserve(64);

        while (!stop_requested_.load(std::memory_order_relaxed))
        {
            // Wait for a signal
            EventfdReadOp op(*this);
            co_await op;

            // We are now awake.
            // Process tasks in batches
            bool working = true;
            while (working)
            {
                items.clear();
                incoming_.try_dequeue_bulk(std::back_inserter(items), 64);

                for (auto& work : items)
                {
                    try
                    {
                        work();
                    }
                    catch (...)
                    {
                        // Swallow exceptions in background tasks
                    }
                }

                // Check if the queue is effectively empty
                if (incoming_.size_approx() == 0)
                {
                    active_.store(false, std::memory_order_seq_cst);

                    //  Double-Check Race Condition
                    // A task might have been enqueued right after we checked size,
                    // but before we stored active_ = false.
                    if (incoming_.size_approx() > 0)
                    {
                        // There is actually work! Mark active again and continue.
                        active_.store(true, std::memory_order_seq_cst);
                        working = true;
                    }
                    else
                    {
                        working = false;
                    }
                }
                else
                {
                    // Queue doesn't empty, yield to let I/O run, then continue processing.
                    wake();
                    working = false;
                }
            }
        }

        // Final drain
        items.clear();
        incoming_.try_dequeue_bulk(std::back_inserter(items), 1024);
        for (auto& work : items)
            work();
    }

    void run_thread(const bool pin, const size_t cpu_index)
    {
        if (pin)
        {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_index % std::thread::hardware_concurrency(), &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        }

        running_.store(true, std::memory_order_release);

        // Ensure active is false initially, so the first schedule wakes us
        active_.store(false, std::memory_order_release);

        exec_.spawn(eventfd_listener());

        while (!stop_requested_.load(std::memory_order_relaxed) || exec_.pending() > 0)
        {
            if (!exec_.run_once(exec_.pending() > 0))
            {
                if (!stop_requested_.load(std::memory_order_relaxed))
                {
                    std::this_thread::yield();
                }
            }
        }
        running_.store(false, std::memory_order_release);
    }

public:
    explicit ThreadContext(const size_t index, const ExecutorConfig cfg = ExecutorConfig{})
        : exec_(cfg), eventfd_(eventfd(0, EFD_NONBLOCK)), index_(index)
    {
        if (eventfd_ < 0)
        {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
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
        incoming_.enqueue(std::move(work));

        if (const bool was_active = active_.exchange(true, std::memory_order_seq_cst); !was_active)
        {
            wake();
        }
    }

    void schedule(Task<> task)
    {
        auto handle = task.release();
        schedule([handle]() { handle.resume(); });
    }

    void wake() const
    {
        constexpr uint64_t val = 1;
        ::write(eventfd_, &val, sizeof(val));
    }

private:
    void start(bool pin)
    {
        thread_ = std::thread([this, pin]() { run_thread(pin, index_); });
    }

    void request_stop()
    {
        stop_requested_.store(true, std::memory_order_release);
        wake();
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
};

class Runtime
{
    std::vector<std::unique_ptr<ThreadContext>> threads_;
    std::atomic<size_t> next_thread_{0};
    bool started_ = false;

public:
    explicit Runtime(RuntimeConfig cfg = RuntimeConfig{})
    {
        if (cfg.num_threads == 0)
            cfg.num_threads = 1;
        threads_.reserve(cfg.num_threads);
        for (size_t i = 0; i < cfg.num_threads; ++i)
        {
            threads_.push_back(std::make_unique<ThreadContext>(i, cfg.executor_config));
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

    /**
     * @brief Run until stopped
     * @param pin_threads
     */
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
               std::any_of(threads_.begin(), threads_.end(), [](const auto& ctx) { return ctx->running(); });
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