#ifndef KIO_BATON_H
#define KIO_BATON_H

#include "kio/core/coro.h"
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

    void Post() noexcept;

    [[nodiscard]] bool IsReady() const noexcept
    {
        return state_.load(std::memory_order_acquire) == static_cast<const void*>(this);
    }

    void Reset() noexcept
    {
        void* expected = this;
        // Only reset if currently signaled.
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
                    return false;  // Already signaled, resume immediately
                }

                next = static_cast<Awaiter*>(old_head);

                if (baton.state_.compare_exchange_weak(old_head, this, std::memory_order_release,
                                                       std::memory_order_acquire))
                {
                    return true;
                }
            }
        }

        void await_resume() noexcept {}
    };

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

        // The timeout task is fire-and-forget but tied to this awaiter's lifetime
        std::optional<Task<void>> timeout_task;

        TimedAwaiter(AsyncBaton& b, io::Worker& w, std::chrono::nanoseconds t) : baton(b), worker(&w), timeout(t) {}

        bool await_ready() const noexcept { return baton.IsReady() || timeout <= std::chrono::nanoseconds::zero(); }

        bool await_suspend(std::coroutine_handle<> coro_handle);

        bool await_resume() noexcept
        {
            const State current = state.load(std::memory_order_acquire);
            if (current == State::kSignaled)
                return true;
            if (current == State::kTimedOut)
                return false;
            return baton.IsReady();
        }

        bool TryComplete(const State new_state) noexcept
        {
            auto expected = State::kWaiting;
            return state.compare_exchange_strong(expected, new_state, std::memory_order_acq_rel);
        }
    };

    [[nodiscard]] Awaiter Wait(io::Worker& worker) noexcept { return Awaiter{*this, worker}; }

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