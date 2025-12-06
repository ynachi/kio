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

#include "../../core/coro.h"
#include "../../core/worker.h"

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
        void notify() noexcept
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
                    owner_.post(handle);
                }
            }
        }

        /**
         * @brief Check if notified.
         */
        [[nodiscard]]
        bool ready() const noexcept
        {
            return state_.load(std::memory_order_acquire) == SET;
        }

        /**
         * @brief Reset for reuse.
         * * MUST be called after notify() completes and waiter has resumed.
         * Only safe to call from the owner Worker thread.
         */
        void reset() noexcept
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
        auto wait() noexcept
        {
            struct Awaiter
            {
                AsyncBaton& baton_;

                bool await_ready() const noexcept { return baton_.state_.load(std::memory_order_acquire) == SET; }

                bool await_suspend(std::coroutine_handle<> h) noexcept
                {
                    // 1. Store handle
                    baton_.waiter_.store(h, std::memory_order_relaxed);

                    // 2. Ensure handle write is visible before state change
                    std::atomic_thread_fence(std::memory_order_release);

                    // 3. Try to transition NOT_SET -> WAITING
                    if (uint8_t expected = NOT_SET; baton_.state_.compare_exchange_strong(expected, WAITING, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        return true;  // Suspend
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
        Task<bool> wait_for(std::chrono::nanoseconds timeout)
        {
            co_await io::SwitchToWorker(owner_);

            struct TimeoutState
            {
                AsyncBaton& baton;
                std::chrono::nanoseconds duration;

                uint64_t timer_op_id = static_cast<uint64_t>(-1);
                struct __kernel_timespec ts{};
                bool timer_submitted = false;
                bool baton_signaled = false;
                std::atomic<bool> resumed{false};

                bool await_ready() noexcept
                {
                    if (baton.state_.load(std::memory_order_acquire) == SET)
                    {
                        baton_signaled = true;
                        return true;
                    }
                    return false;
                }

                bool await_suspend(std::coroutine_handle<> h) noexcept
                {
                    // Step 1: Register with baton FIRST
                    baton.waiter_.store(h, std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_release);

                    uint8_t expected = NOT_SET;
                    if (!baton.state_.compare_exchange_strong(expected, WAITING, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        // Baton already SET - no timeout needed
                        baton_signaled = true;
                        return false;
                    }

                    // Step 2: Set up timer (baton is now in WAITING state)
                    timer_op_id = baton.owner_.get_op_id();
                    baton.owner_.init_op_slot(timer_op_id, h);

                    ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                    ts.tv_nsec = (duration % std::chrono::seconds(1)).count();

                    io_uring_sqe* sqe = io_uring_get_sqe(&baton.owner_.get_ring());
                    if (!sqe)
                    {
                        // Failed to get SQE - must unregister from baton
                        baton.owner_.release_op_id(timer_op_id);
                        timer_op_id = static_cast<uint64_t>(-1);

                        uint8_t state = WAITING;
                        if (baton.state_.compare_exchange_strong(state, NOT_SET, std::memory_order_acq_rel, std::memory_order_acquire))
                        {
                            // Successfully unregistered - immediate timeout
                            baton_signaled = false;
                            return false;
                        }

                        // Baton was signaled while we were cleaning up
                        baton_signaled = true;
                        return false;
                    }

                    io_uring_prep_timeout(sqe, &ts, 0, 0);
                    io_uring_sqe_set_data64(sqe, timer_op_id);
                    timer_submitted = true;

                    return true;
                }

                void await_resume()
                {
                    // Protect against double-resume
                    if (resumed.exchange(true, std::memory_order_acq_rel))
                    {
                        // Already resumed once, this is a spurious resume
                        return;
                    }

                    // Check if result was already determined in await_suspend
                    if (!timer_submitted)
                    {
                        return;
                    }

                    // Determine who resumed us by checking baton state
                    const bool baton_is_set = baton.state_.load(std::memory_order_acquire) == SET;

                    if (baton_is_set)
                    {
                        // Baton signaled us
                        baton_signaled = true;

                        // CRITICAL: We cannot safely cancel the timer because:
                        // 1. The timer CQE might already be in the completion queue
                        // 2. io_uring_prep_timeout_remove creates a NEW CQE with different op_id
                        // 3. The original timer CQE will still arrive with timer_op_id
                        //
                        // Solution: Let the timer complete naturally and ignore its CQE
                        // by replacing the handler with noop_coroutine()
                        baton.owner_.init_op_slot(timer_op_id, std::noop_coroutine());

                        // DO NOT attempt to cancel - just let it timeout naturally
                        // The noop handler will safely consume the CQE when it arrives
                    }
                    else
                    {
                        // We were resumed by timer CQE
                        // Try to unregister from baton
                        uint8_t expected = WAITING;
                        if (baton.state_.compare_exchange_strong(expected, NOT_SET, std::memory_order_acq_rel, std::memory_order_acquire))
                        {
                            // Successfully unregistered - pure timeout
                            baton_signaled = false;
                        }
                        else
                        {
                            // CAS failed - baton was SET concurrently
                            // Report timeout (we're processing timer CQE)
                            // Second resume from task queue will be caught by double-resume check
                            baton_signaled = false;
                        }
                        // timer_op_id will be released by worker when processing the CQE
                    }

                    // Mark timer as no longer active
                    timer_submitted = false;
                }

                bool was_signaled() const noexcept { return baton_signaled; }
            };

            TimeoutState state{*this, timeout};
            co_await state;
            co_return state.was_signaled();
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

        struct TimeoutAwaiter
        {
            AsyncBaton& baton;
            std::chrono::nanoseconds duration;
            bool& timer_won_out;
            bool& baton_won_out;

            // Internal state
            uint64_t timer_op_id = -1;
            // Stable storage for io_uring to read
            struct __kernel_timespec ts{};

            bool await_ready() const noexcept
            {
                // If we are in a race cleanup loop (Timer won but Baton posted),
                // we are NOT ready and must suspend to catch the Baton post.
                if (timer_won_out) return false;
                return baton.ready();
            }

            bool await_suspend(std::coroutine_handle<> h) noexcept
            {
                if (timer_won_out)
                {
                    // Cleanup pass: We know Baton posted us. Just suspend and wait for it.
                    return true;
                }

                // 1. Prepare Timer
                timer_op_id = baton.owner_.get_op_id();
                baton.owner_.init_op_slot(timer_op_id, h);

                ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                ts.tv_nsec = (duration % std::chrono::seconds(1)).count();

                io_uring_sqe* sqe = io_uring_get_sqe(&baton.owner_.get_ring());
                if (sqe)
                {
                    io_uring_prep_timeout(sqe, &ts, 0, 0);
                    io_uring_sqe_set_data64(sqe, timer_op_id);
                    // Note: Worker loop submits automatically, or we rely on implicit submission
                    // from previous ops. Usually safe in kio architecture.
                }
                else
                {
                    // Ring full fallback: treat as immediate timeout
                    baton.owner_.release_op_id(timer_op_id);
                    timer_won_out = true;
                    return false;
                }

                // 2. Register Baton
                baton.waiter_.store(h, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);

                uint8_t expected = NOT_SET;
                if (baton.state_.compare_exchange_strong(expected, WAITING, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return true;  // Successfully suspended
                }

                // Baton is SET (Race: notify happened during setup).
                // Cancel timer immediately.
                // We replace the callback with noop so the CQE is harmless.
                baton.owner_.init_op_slot(timer_op_id, std::noop_coroutine());

                // Try to remove the timeout from kernel to be clean
                io_uring_sqe* cancel_sqe = io_uring_get_sqe(&baton.owner_.get_ring());
                if (cancel_sqe)
                {
                    io_uring_prep_timeout_remove(cancel_sqe, timer_op_id, 0);
                    io_uring_sqe_set_data64(cancel_sqe, baton.owner_.get_op_id());  // dummy op_id for remove
                }

                return false;  // Resume immediately
            }

            void await_resume()
            {
                if (baton.ready())
                {
                    // Baton Signaled!
                    baton_won_out = true;

                    // If we started a timer, we must detach it
                    if (timer_op_id != (uint64_t) -1)
                    {
                        // Detach handler so future timeout/cancel CQE doesn't resume us
                        baton.owner_.init_op_slot(timer_op_id, std::noop_coroutine());

                        // Request kernel removal
                        io_uring_sqe* sqe = io_uring_get_sqe(&baton.owner_.get_ring());
                        if (sqe)
                        {
                            io_uring_prep_timeout_remove(sqe, timer_op_id, 0);
                            // We grab a throwaway op_id for the removal just to keep accounting straight
                            io_uring_sqe_set_data64(sqe, baton.owner_.get_op_id());
                        }
                    }
                }
                else
                {
                    // Baton not ready. Must be Timer?
                    // Try to unregister from Baton.
                    uint8_t expected = WAITING;
                    if (baton.state_.compare_exchange_strong(expected, NOT_SET, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        // Successfully removed from Baton. Pure Timeout.
                        timer_won_out = true;
                        // Timer op_id is released by Worker loop when processing CQE
                    }
                    else
                    {
                        // CAS Failed. Baton is SET.
                        // This means Notify happened concurrently and posted us to Task Queue.
                        // We are currently running (via Timer).
                        // We MUST loop again to consume the pending Baton resume.
                        timer_won_out = true;
                        // baton_won_out stays false for now, ensuring loop continues
                    }
                }
            }
        };
    };

}  // namespace kio::sync

#endif  // KIO_BATON_H
