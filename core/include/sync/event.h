
//
// Created by Yao ACHI on 22/11/2025.
//

#ifndef KIO_EVENT_H
#define KIO_EVENT_H
#include <atomic>
#include <cassert>
#include <coroutine>

#include "core/include/io/worker.h"

namespace kio::sync
{

    /**
     * @brief A synchronization primitive that allows MULTIPLE coroutines to suspend
     * until it is signaled (notified).
     *
     * - Single Consumer? NO (Multiple coroutines can wait)
     * - Single Producer? NO (Any thread can notify)
     * - Allocation Free? YES (Uses intrusive list on coroutine frames)
     * * Use this for 1-to-N signaling (broadcast).
     */
    class AsyncEvent
    {
        struct Awaiter
        {
            AsyncEvent& event;
            Awaiter* next = nullptr;
            std::coroutine_handle<> handle;

            bool await_ready() const noexcept
            {
                // If the state points to SET_SENTINEL, we don't need to suspend
                return event.state_.load(std::memory_order_acquire) == &AsyncEvent::SET_SENTINEL;
            }

            bool await_suspend(std::coroutine_handle<> h) noexcept
            {
                handle = h;

                // 2. CAS Loop to push ourselves onto the stack
                void* old_head = event.state_.load(std::memory_order_acquire);

                while (true)
                {
                    if (old_head == &AsyncEvent::SET_SENTINEL)
                    {
                        // Event happened concurrently while we were preparing.
                        // Don't suspend, resume immediately.
                        return false;
                    }

                    // Link our 'next' to the current head
                    next = static_cast<Awaiter*>(old_head);

                    // Try to swap head to point to us
                    // The acq_rel here ensures:
                    // - Release: Our writes (handle, next) are visible to notify()
                    // - Acquire: We see all previous waiters in the list
                    if (event.state_.compare_exchange_weak(old_head, this, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        // Success, we are now in the list and will be woken up by notify()
                        return true;
                    }
                    // CAS failed, 'old_head' was updated by another thread.
                    // Loop and try again with the new head.
                }
            }

            void await_resume() const noexcept {}
        };

        // State pointer can be:
        // - nullptr: Not set, no waiters.
        // - &SET_SENTINEL: Set (signaled).
        // - Any other pointer: Head of the linked list of Awaiters.
        std::atomic<void*> state_{nullptr};

        // A dummy variable to take the address of for the "SET" state.
        static inline char SET_SENTINEL;

        io::Worker& owner_;

    public:
        explicit AsyncEvent(io::Worker& owner) : owner_(owner) {}

        AsyncEvent(const AsyncEvent&) = delete;
        AsyncEvent& operator=(const AsyncEvent&) = delete;

        /**
         * @brief Notify the event. ALL waiting coroutines will be resumed.
         * Future calls to wait() will complete immediately until reset() is called.
         * * Safe to call from any thread.
         */
        void notify() noexcept
        {
            // Atomically swap state to SET_SENTINEL.
            // Acq/Rel ensures we see writes from waiters, and they see ours.
            void* old_head = state_.exchange(&SET_SENTINEL, std::memory_order_acq_rel);

            if (old_head == &SET_SENTINEL)
            {
                return;  // Already set
            }

            // old_head is the start of our linked list of waiters
            auto* curr = static_cast<Awaiter*>(old_head);

            while (curr)
            {
                // CRITICAL: We must read 'next' BEFORE posting the handle.
                // Once we post, the worker might resume the coroutine and destroy
                // the 'curr' Awaiter object immediately (it lives on the stack).
                auto* next = curr->next;

                // Schedule resumption on the worker
                if (curr->handle)
                {
                    owner_.post(curr->handle);
                }

                curr = next;
            }
        }

        /**
         * @brief Check if the event is currently set.
         */
        [[nodiscard]]
        bool is_set() const noexcept
        {
            return state_.load(std::memory_order_acquire) == &SET_SENTINEL;
        }

        /**
         * @brief Reset the event to the not-set state.
         * * NOT thread-safe vs. notify() or wait().
         * Should be called by the coordinator when it is known that no one is waiting.
         */
        void reset() noexcept
        {
            // We assume no one is currently modifying the list during reset.
            // The only valid transition is SET -> NOT_SET (nullptr).
            if (void* expected = &SET_SENTINEL; !state_.compare_exchange_strong(expected, nullptr, std::memory_order_release))
            {
                // If this fails, it means either:
                // 1. State was already nullptr (fine)
                // 2. State was a list of waiters (BAD! Logic error in user code)
                assert(expected == nullptr && "reset() called while coroutines are waiting!");
            }
        }

        /**
         * @brief Suspend until notified.
         */
        [[nodiscard]]
        auto wait() noexcept
        {
            return Awaiter{*this};
        }
    };

}  // namespace kio::sync
#endif  // KIO_EVENT_H
