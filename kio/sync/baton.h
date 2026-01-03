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

    // Non-copyable/movable to ensure pointer stability (state_ stores 'this')
    AsyncBaton(const AsyncBaton&) = delete;
    AsyncBaton& operator=(const AsyncBaton&) = delete;
    AsyncBaton(AsyncBaton&&) = delete;
    AsyncBaton& operator=(AsyncBaton&&) = delete;

    ~AsyncBaton()
    {
        // Debug check: Destroying a baton while coroutines are still suspended
        // waiting on it is undefined behavior (Use-After-Free risk).
        // The state must be either Signaled (this) or Empty (nullptr).
        void* current_state = state_.load(std::memory_order_relaxed);
        assert((current_state == static_cast<void*>(this) || current_state == nullptr) &&
               "Destroying AsyncBaton while coroutines are still waiting!");
    }

    /**
     * @brief Signals the baton.
     * Resumes ALL currently waiting coroutines on their respective Worker threads.
     * Thread-safe, can be called from any thread.
     * * Any future awaiters will proceed immediately until Reset() is called.
     */
    void Post() noexcept
    {
        void* const kSignaledState = this;

        // Atomically swap to signaled state
        void* old_state = state_.exchange(kSignaledState, std::memory_order_acq_rel);

        if (old_state != kSignaledState && old_state != nullptr)
        {
            // We are the winner who transitioned to Signaled.
            // 'old_state' is the head of the linked list of waiters.
            // Walk the list and resume everyone on their Worker.
            auto* awaiter = static_cast<Awaiter*>(old_state);
            while (awaiter != nullptr)
            {
                auto* next = awaiter->next;
                // Resume on the awaiter's original Worker (thread affinity)
                io::internal::WorkerAccess::Post(*awaiter->worker, awaiter->handle);
                awaiter = next;
            }
        }
        // If old_state == signaled_state: Already signaled, idempotent
        // If old_state == nullptr: No waiters, just became signaled
    }

    /**
     * @brief Checks if signaled without suspending.
     * Thread-safe, can be called from any thread.
     */
    [[nodiscard]] bool IsReady() const noexcept
    {
        return state_.load(std::memory_order_acquire) == static_cast<const void*>(this);
    }

    /**
     * @brief Resets the baton to not-signaled state.
     * * Only transitions from Signaled -> Empty.
     * If concurrent operations are ongoing, this may be a no-op.
     * * @warning Not thread-safe relative to concurrent Wait() operations.
     * Should typically be called after all waiters have been resumed
     * and completed their work.
     */
    void Reset() noexcept
    {
        // Use CAS to ensure we only reset from Signaled state
        void* expected = this;
        state_.compare_exchange_strong(
            expected, 
            nullptr,
            std::memory_order_acq_rel,   // Success: publish the reset
            std::memory_order_relaxed);  // Failure: concurrent operation
    }

    /**
     * @brief Awaitable for suspending until signaled.
     * * Each awaiter is allocated on the coroutine frame and forms a node
     * in the intrusive lock-free linked list.
     */
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
            void* old_head = baton.state_.load(std::memory_order_acquire);

            // CAS loop to push ourselves onto the head of the list
            while (true)
            {
                if (old_head == signaled_state)
                {
                    // Became signaled while we were preparing to wait.
                    // Return false to resume immediately without suspending.
                    return false;
                }

                // Link to the previous head
                next = static_cast<Awaiter*>(old_head);

                if (baton.state_.compare_exchange_weak(
                        old_head,
                        this,
                        std::memory_order_release,   // Success: publish awaiter
                        std::memory_order_acquire))  // Failure: reload state
                {
                    // Successfully added to list. Suspend.
                    return true;
                }
            }
        }

        void await_resume() noexcept {}
    };

    /**
     * @brief Create an awaiter bound to a specific Worker.
     * @param worker The Worker thread this coroutine belongs to.
     */
    [[nodiscard]] Awaiter Wait(io::Worker& worker) noexcept
    {
        return Awaiter{*this, worker};
    }

    /**
     * @brief Convenience operator for single-worker use cases.
     * @deprecated Use Wait(worker) for explicit worker binding.
     */
    [[nodiscard]] auto operator co_await() = delete;
    // Deleted to force explicit Worker binding via Wait(worker)

private:
    std::atomic<void*> state_;
};

} // namespace kio::sync

#endif // KIO_BATON_H