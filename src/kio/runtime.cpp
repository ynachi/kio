#include "kio/runtime.hpp"
#include "kio/executor.hpp"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/eventfd.h>

namespace kio
{

namespace detail
{
// ----------------------------------------------------------------------------
// BlockingThreadPool Implementation
// ----------------------------------------------------------------------------

BlockingThreadPool::BlockingThreadPool(size_t threads, const size_t max_queue)
    : queue_(max_queue),
      slots_available_(max_queue),
      tasks_available_(0),
max_queue_(max_queue)
{
    if (threads == 0)
    {
        threads = 1;
    }
    workers_.reserve(threads);
    for (size_t i = 0; i < threads; ++i)
    {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

BlockingThreadPool::~BlockingThreadPool()
{
    stop_.store(true, std::memory_order_release);

    // Wake up all workers so they can exit.
    tasks_available_.release(workers_.size());

    for (auto& t : workers_)
        if (t.joinable())
        {
            t.join();
        }
}

bool BlockingThreadPool::try_submit(Job job)
{
    if (stop_.load(std::memory_order_acquire))
        return false;

    // Try to acquire a "free slot" token without blocking.
    if (!slots_available_.try_acquire())
    {
        return false;
    }

    if (!queue_.try_push(std::move(job)))
    {
        // If enqueue fails (e.g. OOM), return the token
        slots_available_.release();
        return false;
    }

    // Wake up a worker
    tasks_available_.release();
    return true;
}

size_t BlockingThreadPool::get_queue_size() const
{
    return max_queue_;
}

void BlockingThreadPool::worker_loop()
{
    while (true)
    {
        // Wait for work (or shutdown signal)
        tasks_available_.acquire();

        if (stop_.load(std::memory_order_acquire))
        {
            return;
        }

        auto run = [&](Job&& job) noexcept
        {
            try { job(); } catch (...) {}
        };

        if (queue_.try_consume_one(run))
        {
            slots_available_.release();
        }
    }
}

}  // namespace detail

// ----------------------------------------------------------------------------
// ThreadContext Implementation
// ----------------------------------------------------------------------------

ThreadContext::ThreadContext(const size_t index,
                             const RuntimeConfig cfg,
                             detail::BlockingThreadPool* blocking,
                             const size_t task_queue_cap
                             )
    : eventfd_(eventfd(0, EFD_CLOEXEC)),
      incoming_(task_queue_cap),
      blocking_(blocking),
      index_(index)
{
    detail::ExecutorConfig excfg;
    excfg.entries = cfg.entries;
    excfg.uring_flags = cfg.uring_flags;
    excfg.sq_thread_idle_ms = cfg.sq_thread_idle_ms;
    new (&exec_) detail::Executor(excfg);

    if (eventfd_ < 0)
    {
        throw std::system_error(errno, std::system_category(), "eventfd");
    }
}

ThreadContext::ThreadContext(const size_t index,
                             const detail::ExecutorConfig cfg,
                             detail::BlockingThreadPool* blocking,
                             const size_t task_queue_cap
                             )
    : exec_(cfg),
      eventfd_(eventfd(0, EFD_CLOEXEC)),
      incoming_(task_queue_cap),
      blocking_(blocking),
      index_(index)

{
    if (eventfd_ < 0)
    {
        throw std::system_error(errno, std::system_category(), "eventfd");
    }
}

ThreadContext::~ThreadContext()
{
    if (eventfd_ >= 0)
    {
        ::close(eventfd_);
    }
}

void ThreadContext::schedule(WorkItem work)
{
    // NOTE: in our MpscRing, the move into the slot happens only on success.
    int spins = 0;
    while (!incoming_.try_push(std::move(work)))
    {
        // Overload: produces back off. Keep it simple for now
        // TODO: make it configurable ?
        if (++spins < 64)
        {
            ; // tigh spin a bit
        } else
        {
            std::this_thread::yield();
        }
    }

    // Now account + wake-on-transition

    // Only wake on transition 0 -> 1.
    if (const size_t prev = pending_work_.fetch_add(1, std::memory_order_release); prev != 0)
    {
        return;
    }

    if (ThreadContext* src = current(); src != nullptr && src->runtime_ == runtime_)
    {
        // do not wakup self
        if (src != this)
        {
            wake_msg_ring_from(*src);
        }
    }
    else
    {
        // Foreign thread: use eventfd fallback (park op will complete and wake the ring).
        wake_eventfd();
    }
}

void ThreadContext::schedule(Task<> task)
{
    auto handle = task.release();
    schedule([handle]() { handle.resume(); });
}

void ThreadContext::wake_eventfd() const noexcept
{
    constexpr uint64_t val = 1;
    for (;;)
    {
        const ssize_t n = ::write(eventfd_, &val, sizeof(val));
        if (std::cmp_equal(n, sizeof(val)))
        {
            return;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        // Saturation (EAGAIN) or other errors: best-effort wake.
        return;
    }
}

size_t ThreadContext::drain_incoming(const size_t max_items)
{


    size_t n = 0;

    auto run = [&](WorkItem&& w) noexcept
    {
        try { w(); } catch (...) {}
    };

    while (n < max_items && incoming_.try_consume_one(run))
    {
        ++n;
    }

    if (n > 0)
    {
        pending_work_.fetch_sub(n, std::memory_order_seq_cst);
    }

    return n;
}

void ThreadContext::wake_msg_ring_from(ThreadContext& source) const noexcept
{
    source.exec_.msg_ring_wake(exec_.ring_fd(), 1, 0);
}

void ThreadContext::run_thread(const bool pin, const size_t cpu_index)
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

    while (!stop_requested_.load(std::memory_order_relaxed) ||
           exec_.pending() > 0 ||
           pending_work_.load(std::memory_order_acquire) > 0)
    {
        const bool have_work = pending_work_.load(std::memory_order_acquire) > 0;

        // If we have queued work, don't block waiting for I/O; poll completions and run work.
        // If we don't have work, it's fine to block (park op + msg_ring will wake us).
        (void)exec_.loop_once(!have_work);

        // Run queued work.
        // TODO : make configurabe ?
        (void)drain_incoming(64);

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

void ThreadContext::start(bool pin)
{
    thread_ = std::thread([this, pin]() { run_thread(pin, index_); });
}

void ThreadContext::request_stop()
{
    stop_requested_.store(true, std::memory_order_release);
    // request_stop is typically called from a foreign thread; eventfd is the safe wake.
    wake_eventfd();
}

void ThreadContext::join()
{
    if (thread_.joinable())
    {
        thread_.join();
    }
}

// ----------------------------------------------------------------------------
// ScheduleOnAwaiter Implementation
// ----------------------------------------------------------------------------

void ScheduleOnAwaiter::await_suspend(std::coroutine_handle<> h) const
{
    target_.schedule([h]() { h.resume(); });
}

// ----------------------------------------------------------------------------
// Runtime Implementation
// ----------------------------------------------------------------------------

Runtime::Runtime(RuntimeConfig cfg)
    : blocking_pool_(cfg.blocking_threads, cfg.blocking_queue)
{
    if (cfg.num_threads == 0)
        cfg.num_threads = 1;

    // Map RuntimeConfig options to internal ExecutorConfig
    detail::ExecutorConfig excfg;
    excfg.entries = cfg.entries;
    excfg.uring_flags = cfg.uring_flags;
    excfg.sq_thread_idle_ms = cfg.sq_thread_idle_ms;

    threads_.reserve(cfg.num_threads);
    for (size_t i = 0; i < cfg.num_threads; ++i)
    {
        auto ctx = std::make_unique<ThreadContext>(i, excfg, &blocking_pool_, cfg.task_queue_capacity);
        ctx->runtime_ = this;
        threads_.push_back(std::move(ctx));
    }
}

Runtime::~Runtime()
{
    stop();
}

void Runtime::loop_forever(const bool pin_threads)
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

void Runtime::stop()
{
    if (!started_)
        return;

    for (const auto& ctx : threads_)
        ctx->request_stop();
    for (const auto& ctx : threads_)
        ctx->join();

    started_ = false;
}

}  // namespace uring