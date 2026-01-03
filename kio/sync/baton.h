//
// Created by Yao ACHI on 22/11/2025.
//

#ifndef KIO_BATON_H
#define KIO_BATON_H

#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <liburing.h>

#include "kio/core/coro.h"
#include "kio/core/worker.h"

namespace kio::sync
{

/**
 * @brief Lock-free synchronization primitive for Kio coroutines.
 * Allows cross-thread signaling that safely resumes coroutines on their
 * owning Worker thread. This is a 1-to-1 syn primitive.
 */
class AsyncBaton
{
public:
    explicit AsyncBaton(io::Worker& owner) : owner_(owner) {}

    AsyncBaton(const AsyncBaton&) = delete;
    AsyncBaton& operator=(const AsyncBaton&) = delete;
    AsyncBaton(AsyncBaton&&) = delete;
    AsyncBaton& operator=(AsyncBaton&&) = delete;

    /**
     * @brief Signal the baton from any thread.
     * If a coroutine is waiting, it will be scheduled on the owner Worker.
     * Idempotent - multiple calls are safe.
     */
    void Notify() noexcept
    {
        // Exchange returns the *previous* value.
        // Acq/Rel ensures the visibility of data protected by this baton.
        if (const uint8_t prev = state_.exchange(SET, std::memory_order_acq_rel); prev == WAITING)
        {
            // We are the one effectively "waking" the waiter.
            // This fence ensures that the writing to 'waiter_' in wait()
            // is visible to us now.
            std::atomic_thread_fence(std::memory_order_acquire);

            if (const auto handle = waiter_.load(std::memory_order_relaxed))
            {
                io::internal::WorkerAccess::Post(owner_, handle);
            }
        }
    }

    /**
     * @brief Check if notified.
     */
    [[nodiscard]]
    bool Ready() const noexcept
    {
        return state_.load(std::memory_order_acquire) == SET;
    }

    /**
     * @brief Reset for reuse.
     * * MUST be called after notify() completes and waiter has resumed.
     * Only safe to call from the owner Worker thread.
     */
    void Reset() noexcept
    {
        const uint8_t current = state_.load(std::memory_order_acquire);
        assert(current != WAITING && "reset() called while coroutine is suspended!");

        state_.store(NOT_SET, std::memory_order_release);
        waiter_.store(nullptr, std::memory_order_relaxed);
    }

    /**
     * @brief Suspend until notified.
     * * MUST be called from owner Worker thread.
     */
    [[nodiscard]]
    auto Wait() noexcept
    {
        struct Awaiter
        {
            AsyncBaton& baton_;

            bool AwaitReady() const noexcept { return baton_.state_.load(std::memory_order_acquire) == SET; }

            bool AwaitSuspend(std::coroutine_handle<> h) noexcept
            {
                baton_.waiter_.store(h, std::memory_order_relaxed);

                std::atomic_thread_fence(std::memory_order_release);

                if (uint8_t expected = NOT_SET; baton_.state_.compare_exchange_strong(
                            expected, WAITING, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return true;
                }

                // CAS failed. Since we are the only consumer, this MUST mean
                // concurrent notify() happened and state is now SET.
                return false;
            }

            void await_resume() const noexcept {}
        };

        return Awaiter{*this};
    }

    /**
     * @brief Suspend until notified or timeout expires.
     *
     * @param timeout Duration to wait.
     * @return true if signaled (baton set), false if timeout occurred.
     */
    Task<bool> WaitFor(std::chrono::nanoseconds timeout)
    {
        co_await io::SwitchToWorker(owner_);

        // 1. Setup stable state on the stack.
        // Because wait_for is a coroutine, these variables persist across suspension points.
        io::IoCompletion timer_completion{};
        timer_completion.result = 1;  // Sentinel value (io_uring returns negative errors)

        __kernel_timespec ts{};
        ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
        ts.tv_nsec = (timeout % std::chrono::seconds(1)).count();

        bool timer_submitted = false;

        // 2. Awaiter that initiates both the Baton wait and the Timer
        struct RaceAwaiter
        {
            AsyncBaton& baton;
            io::IoCompletion& completion;
            __kernel_timespec& ts;
            bool& submitted;

            bool await_ready() const noexcept { return baton.Ready(); }

            bool await_suspend(std::coroutine_handle<> h)
            {
                // Register with Baton first
                baton.waiter_.store(h, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);

                // Try to transition to WAITING
                if (uint8_t expected = NOT_SET; !baton.state_.compare_exchange_strong(
                            expected, WAITING, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    // Baton was already SET, resume immediately
                    return false;
                }

                // Submit Timer
                auto& ring = io::internal::WorkerAccess::GetRing(baton.owner_);
                io_uring_sqe* sqe = io_uring_get_sqe(&ring);

                if (!sqe)
                {
                    // Ring full. Try to rollback baton state to allow immediate resume (simulate error/ready)
                    if (uint8_t val = WAITING; baton.state_.compare_exchange_strong(
                                val, NOT_SET, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        // Successfully unregistered, treat as immediate timeout due to resource exhaustion
                        return false;
                    }
                    // Baton was set concurrently, treat as success
                    return false;
                }

                completion.handle = h;
                io_uring_prep_timeout(sqe, &ts, 0, 0);
                io_uring_sqe_set_data(sqe, &completion);
                submitted = true;

                return true;
            }

            void await_resume() {}
        };

        co_await RaceAwaiter{*this, timer_completion, ts, timer_submitted};

        // 3. Handle the Race Result
        // We are now running again. Check if the timer is what woke us up.
        // If timer_completion.result is still 1, the timer has NOT run.
        if (timer_submitted && timer_completion.result == 1)
        {
            // The timer is still pending in the io_uring or kernel.
            // We must cancel it and wait for the cancellation to complete.
            // This ensures 'timer_completion' (on our stack) is not destroyed while the kernel uses it.
            struct CancelAwaiter
            {
                AsyncBaton& self;
                io::IoCompletion& tc;

                bool await_ready() { return false; }
                bool await_suspend(std::coroutine_handle<> h)
                {
                    auto& ring = io::internal::WorkerAccess::GetRing(self.owner_);
                    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
                    if (sqe)
                    {
                        // Cancel the timeout using the pointer as user_data
                        io_uring_prep_timeout_remove(sqe, reinterpret_cast<__u64>(&tc), 0);
                        // We don't track the cancel op itself, passing nullptr is safe
                        io_uring_sqe_set_data(sqe, nullptr);
                    }

                    // Redirect the TIMER completion to resume us here
                    tc.handle = h;
                    return true;
                }
                void await_resume() {}
            };

            co_await CancelAwaiter{*this, timer_completion};
        }

        // 4. Determine final status
        if (Ready())
        {
            co_return true;
        }

        // If not ready, we timed out.
        // Try to unregister from baton if we are still marked as waiting.
        if (uint8_t expected = WAITING;
            state_.compare_exchange_strong(expected, NOT_SET, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            co_return false;
        }

        // If unregister failed, Baton was set concurrently.
        co_return true;
    }

private:
    enum State : uint8_t
    {
        NOT_SET = 0,
        SET = 1,
        WAITING = 2
    };

    io::Worker& owner_;
    std::atomic<uint8_t> state_{NOT_SET};
    std::atomic<std::coroutine_handle<>> waiter_{nullptr};
};

}  // namespace kio::sync

#endif  // KIO_BATON_H
