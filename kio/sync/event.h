
//
// Created by Yao ACHI on 22/11/2025.
//

#ifndef KIO_EVENT_H
#define KIO_EVENT_H
#include <atomic>
#include <cassert>
#include <coroutine>

#include "../core/worker.h"

namespace kio::sync
{

/**
 * @brief A synchronization primitive that allows MULTIPLE coroutines to suspend
 * until it is signaled (notified).
 *
 * @note Use this for 1-to-N signaling (broadcast)
 * - Single Consumer? NO (Multiple coroutines can wait)
 * - Single Producer? NO (Any thread can notify)
 */
class AsyncEvent
{
    struct Awaiter
    {
        AsyncEvent& event;

        // Make these atomic so publication is explicit and visible to TSAN
        std::atomic<Awaiter*> next{nullptr};

        // store coroutine handle as a pointer (void*) atomically,
        // we can't portably have atomic<coroutine_handle<>> so use address
        std::atomic<void*> handle_ptr{nullptr};

        bool await_ready() const noexcept
        {
            return event.state_.load(std::memory_order_acquire) == &AsyncEvent::SET_SENTINEL;
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept
        {
            // Publish the handle pointer first with release semantics
            handle_ptr.store(h.address(), std::memory_order_release);

            void* old_head = event.state_.load(std::memory_order_acquire);

            while (true)
            {
                if (old_head == &AsyncEvent::SET_SENTINEL)
                {
                    // event was set concurrently. Unpublish handle (optional), don't suspend.
                    handle_ptr.store(nullptr, std::memory_order_release);
                    return false;
                }

                // link to current head (relaxed is fine; visibility is established by CAS below)
                next.store(static_cast<Awaiter*>(old_head), std::memory_order_relaxed);

                if (event.state_.compare_exchange_weak(old_head, this, std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
                {
                    // successfully published (release side): other thread's acquire will see handle_ptr/next
                    return true;
                }
                // CAS failed; 'old_head' was updated; loop and try again
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
        void* old_head = state_.exchange(&SET_SENTINEL, std::memory_order_acq_rel);

        if (old_head == &SET_SENTINEL)
        {
            return;  // Already set
        }

        auto* curr = static_cast<Awaiter*>(old_head);

        while (curr)
        {
            // read next with acquire to observe what the waiter published
            auto* next = curr->next.load(std::memory_order_acquire);

            // read handle pointer
            void* hptr = curr->handle_ptr.load(std::memory_order_acquire);
            if (hptr)
            {
                std::coroutine_handle<> h = std::coroutine_handle<>::from_address(hptr);
                owner_.Post(h);
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
        if (void* expected = &SET_SENTINEL;
            !state_.compare_exchange_strong(expected, nullptr, std::memory_order_release))
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
