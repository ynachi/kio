#ifndef KIO_BATON_H
#define KIO_BATON_H

#include <atomic>
#include <coroutine>
#include <cassert>

#include "kio/core/worker.h"

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
 * @endcode
 */
class AsyncBaton
{
public:
    struct Awaiter;

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
    }

    void Post() noexcept
    {
        void* const kSignaledState = this;

        // Optimization: Double-checked locking pattern (without the lock)
        // If already signaled, avoid the expensive atomic RMW.
        if (state_.load(std::memory_order_acquire) == kSignaledState)
        {
            return;
        }

        // Atomically swap to signaled state
        void* old_state = state_.exchange(kSignaledState, std::memory_order_acq_rel);

        if (old_state != kSignaledState && old_state != nullptr)
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
        state_.compare_exchange_strong(
            expected,
            nullptr,
            std::memory_order_acq_rel,
            std::memory_order_relaxed);
    }

    struct Awaiter
    {
        AsyncBaton& baton;
        std::coroutine_handle<> handle;
        io::Worker* worker;
        Awaiter* next{nullptr};

        explicit Awaiter(AsyncBaton& b, io::Worker& w) : baton(b), worker(&w) {}

        bool await_ready() const noexcept
        {
            return baton.IsReady();
        }

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
                if (baton.state_.compare_exchange_weak(
                        old_head,
                        this,
                        std::memory_order_release,
                        std::memory_order_acquire))
                {
                    return true;
                }
            }
        }

        void await_resume() noexcept {}
    };

    [[nodiscard]] Awaiter Wait(io::Worker& worker) noexcept
    {
        return Awaiter{*this, worker};
    }

    [[nodiscard]] auto operator co_await() = delete;

private:
    std::atomic<void*> state_;
};

} // namespace kio::sync

#endif // KIO_BATON_H