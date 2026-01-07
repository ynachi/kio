#ifndef KIO_BATON_H
#define KIO_BATON_H

#include "kio/core/worker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <mutex>
#include <optional>

namespace kio::sync
{

/**
 * @brief AsyncBaton - Lock-free multi-waiter coroutine synchronization primitive.
 *
 * A synchronization primitive that allows multiple coroutines to wait until
 * another entity posts.
 * * CRITICAL: Maintains Thread Affinity.
 * Waiters are resumed on their original Worker thread to respect
 * io_uring SINGLE_ISSUER constraints.
 *
 * Architecture:
 * - Supports MULTIPLE waiting coroutines (Multi-Consumer).
 * - Waiters form a lock-free LIFO linked list via Awaiter objects.
 * - Each awaiter captures its current Worker to ensure it is resumed there.
 * - As a primitive, this is not cancellation-aware; the user must make sure
 * Post is called before destroying the waiters.
 *
 * Usage patterns:
 *
 *@code
 * // One-shot signal
 * AsyncBaton ready;
 * co_await ready.Wait(worker);
 * ready.Post();
 *
 * // Broadcast to multiple waiters (on different workers)
 * AsyncBaton broadcast;
 * // ... N coroutines await ...
 * broadcast.Post();  // All wake up on their respective threads
 *
 * // Reusable event (manual reset)
 * ready.Post();
 * co_await ready.Wait(worker);  // Proceeds immediately
 * ready.Reset();
 * co_await ready.Wait(worker);  // Blocks until next Post()
 *
 * // Timed wait
 * const bool signaled = co_await ready.WaitFor(worker, std::chrono::milliseconds(50));
 * if (!signaled)
 * {
 *     // timeout
 * }
 * @endcode
 */
class AsyncBaton
{
public:
    struct Awaiter;
    struct TimedAwaiter;

    AsyncBaton() noexcept : state_(nullptr) {}

    AsyncBaton(const AsyncBaton&) = delete;
    AsyncBaton& operator=(const AsyncBaton&) = delete;
    AsyncBaton(AsyncBaton&&) = delete;
    AsyncBaton& operator=(AsyncBaton&&) = delete;

    ~AsyncBaton()
    {
        const void* current_state = state_.load(std::memory_order_relaxed);
        assert((current_state == static_cast<void*>(this) || current_state == nullptr) &&
               "Destroying AsyncBaton while coroutines are still waiting! (Dangling coroutines)");
        std::scoped_lock lock(timed_mutex_);
        assert(timed_head_ == nullptr && "Destroying AsyncBaton while timed coroutines are still waiting!");
    }

    void Post() noexcept
    {
        void* const signaled_state = this;

        // Optimization: Double-checked locking pattern (without the lock)
        // If already signaled, avoid the expensive atomic RMW.
        if (state_.load(std::memory_order_acquire) == signaled_state)
        {
            return;
        }

        // Atomically swap to signaled state

        if (void* old_state = state_.exchange(signaled_state, std::memory_order_acq_rel);
            old_state != signaled_state && old_state != nullptr)
        {
            // Walk the linked list and resume everyone
            const auto* awaiter = static_cast<Awaiter*>(old_state);
            while (awaiter != nullptr)
            {
                const auto* next = awaiter->next;
                // Resume on the awaiter's original Worker (thread affinity)
                io::internal::WorkerAccess::Post(*awaiter->worker, awaiter->handle);
                awaiter = next;
            }
        }

        TimedAwaiter* timed_waiters = TakeTimedWaiters();
        while (timed_waiters != nullptr)
        {
            TimedAwaiter* next = timed_waiters->next;
            if (timed_waiters->TryComplete(TimedAwaiter::State::kSignaled))
            {
                io::internal::WorkerAccess::Post(*timed_waiters->worker, timed_waiters->handle);
            }
            timed_waiters = next;
        }
    }

    [[nodiscard]] bool IsReady() const noexcept
    {
        return state_.load(std::memory_order_acquire) == static_cast<const void*>(this);
    }

    void Reset() noexcept
    {
        void* expected = this;
        // Only reset if currently signaled.
        // If there are waiters (a pointer != this), do not reset.
        state_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    struct Awaiter
    {
        AsyncBaton& baton;
        std::coroutine_handle<> handle;
        io::Worker* worker;
        Awaiter* next{nullptr};

        explicit Awaiter(AsyncBaton& b, io::Worker& w) : baton(b), worker(&w) {}

        bool await_ready() const noexcept { return baton.IsReady(); }

        bool await_suspend(std::coroutine_handle<> coro_handle) noexcept
        {
            handle = coro_handle;
            const void* signaled_state = &baton;

            // We load relaxed first, loop handles the retry logic
            void* old_head = baton.state_.load(std::memory_order_relaxed);

            while (true)
            {
                if (old_head == signaled_state)
                {
                    // Already signaled, resume immediately
                    return false;
                }

                next = static_cast<Awaiter*>(old_head);

                // release: publish 'next' and 'handle'
                // acquire: ensure we see the latest state (signaled or list head)
                if (baton.state_.compare_exchange_weak(old_head, this, std::memory_order_release,
                                                       std::memory_order_acquire))
                {
                    return true;
                }
            }
        }

        void await_resume() noexcept {}
    };

    [[nodiscard]] Awaiter Wait(io::Worker& worker) noexcept { return Awaiter{*this, worker}; }

    struct TimedAwaiter
    {
        enum class State : uint8_t
        {
            kWaiting = 0,
            kSignaled = 1,
            kTimedOut = 2
        };

        AsyncBaton& baton;
        std::coroutine_handle<> handle;
        io::Worker* worker;
        std::chrono::nanoseconds timeout;
        TimedAwaiter* next{nullptr};
        std::atomic<State> state{State::kWaiting};
        std::optional<Task<void>> timeout_task;
        __kernel_timespec timeout_ts{};

        TimedAwaiter(AsyncBaton& b, io::Worker& w, std::chrono::nanoseconds t) : baton(b), worker(&w), timeout(t) {}

        bool await_ready() const noexcept { return baton.IsReady() || timeout <= std::chrono::nanoseconds::zero(); }

        bool await_suspend(std::coroutine_handle<> coro_handle)
        {
            handle = coro_handle;

            if (timeout <= std::chrono::nanoseconds::zero())
            {
                state.store(State::kTimedOut, std::memory_order_release);
                return false;
            }

            if (!baton.EnqueueTimed(this))
            {
                return false;
            }

            timeout_ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
            timeout_ts.tv_nsec = (timeout % std::chrono::seconds(1)).count();

            timeout_task.emplace(
                [this]() -> Task<void>
                {
                    auto prep = [](io_uring_sqe* sqe, __kernel_timespec* t, unsigned flags)
                    { io_uring_prep_timeout(sqe, t, 0, flags); };
                    auto awaitable = io::MakeUringAwaitable(*worker, prep, &timeout_ts, 0);
                    (void)co_await awaitable;
                    if (!TryComplete(State::kTimedOut))
                    {
                        co_return;
                    }
                    baton.RemoveTimed(this);
                    io::internal::WorkerAccess::Post(*worker, handle);
                }());
            timeout_task->h.resume();
            return true;
        }

        bool await_resume() noexcept
        {
            const State current = state.load(std::memory_order_acquire);
            if (current == State::kSignaled)
            {
                return true;
            }
            if (current == State::kTimedOut)
            {
                return false;
            }
            return baton.IsReady();
        }

        bool TryComplete(const State new_state) noexcept
        {
            auto expected = State::kWaiting;
            return state.compare_exchange_strong(expected, new_state, std::memory_order_acq_rel);
        }
    };

    [[nodiscard]] TimedAwaiter WaitFor(io::Worker& worker, std::chrono::nanoseconds timeout) noexcept
    {
        return TimedAwaiter{*this, worker, timeout};
    }

    [[nodiscard]] auto operator co_await() = delete;

private:
    bool EnqueueTimed(TimedAwaiter* awaiter) noexcept
    {
        std::scoped_lock lock(timed_mutex_);
        if (IsReady())
        {
            return false;
        }
        awaiter->next = timed_head_;
        timed_head_ = awaiter;
        return true;
    }

    void RemoveTimed(TimedAwaiter* awaiter) noexcept
    {
        std::scoped_lock lock(timed_mutex_);
        TimedAwaiter** current = &timed_head_;
        while (*current != nullptr)
        {
            if (*current == awaiter)
            {
                *current = awaiter->next;
                return;
            }
            current = &(*current)->next;
        }
    }

    TimedAwaiter* TakeTimedWaiters() noexcept
    {
        std::scoped_lock lock(timed_mutex_);
        TimedAwaiter* head = timed_head_;
        timed_head_ = nullptr;
        return head;
    }

    std::atomic<void*> state_;
    std::mutex timed_mutex_;
    TimedAwaiter* timed_head_{nullptr};
};

}  // namespace kio::sync

#endif  // KIO_BATON_H
