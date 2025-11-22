//
// Created by Yao ACHI on 22/11/2025.
//

#ifndef KIO_BATON_H
#define KIO_BATON_H

// ============================================================================
// kio/include/sync/baton.h
// ============================================================================

#pragma once
#include <atomic>
#include <cassert>
#include <coroutine>

#include "core/include/io/worker.h"

namespace kio::sync
{

    /**
     * @brief Lock-free synchronization primitive for Kio coroutines.
     * Allows cross-thread signaling that safely resumes coroutines on their
     * owning Worker thread. This is a 1-to-1 syn primitive. In case multiple
     * coroutines try to wait on it, only the first one will actually suspend.
     * All the others will immediately resume.
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
                    // We check to be sure (though logically it must be SET).
                    // Don't suspend, resume immediately
                    return false;
                }

                void await_resume() const noexcept {}
            };

            return Awaiter{*this};
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
