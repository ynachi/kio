#pragma once
////////////////////////////////////////////////////////////////////////////////
// utilities/sync.hpp - Async Synchronization Primitives
//
// 1. AsyncBaton: A Multi-Consumer Manual-Reset Event.
//    - Lock-free (Treiber Stack) & Zero-Allocation (Intrusive List) for standard waits.
//    - Mutex-protected list for timed waits (wait_for).
//    - Supports Coroutine, Thread, and Timed waits.
////////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <latch>
#include <memory>
#include <mutex>
#include <vector>

#include "io_pool/runtime.hpp"

namespace uring
{

class AsyncBaton : public std::enable_shared_from_this<AsyncBaton>
{
    // Shared state for the race between Post and Timeout
    struct RaceState {
        std::atomic<int> status{0}; // 0=Waiting, 1=Signaled, 2=TimedOut
        std::coroutine_handle<> waiter;
        ThreadContext* ctx;
    };

    std::mutex timed_mutex_;
    std::vector<std::shared_ptr<RaceState>> timed_waiters_;

public:
    struct Awaiter;

    AsyncBaton() noexcept : state_(nullptr) {}

    // Non-copyable/movable
    AsyncBaton(const AsyncBaton&) = delete;
    AsyncBaton& operator=(const AsyncBaton&) = delete;

    /// @brief Checks if the baton is currently signaled.
    [[nodiscard]] bool is_set() const noexcept
    {
        return state_.load(std::memory_order_acquire) == SET_PTR;
    }

    /// @brief Signals the baton. Wakes up ALL waiters (Coroutines, Threads, Timed).
    void post() noexcept
    {
        // 1. Lock-Free Path: Atomically swap state to SET_PTR.
        void* old_head = state_.exchange(SET_PTR, std::memory_order_acq_rel);

        if (old_head == SET_PTR) return; // Already set

        // Resume all standard waiters
        auto* waiter = static_cast<Awaiter*>(old_head);
        while (waiter)
        {
            auto* next = waiter->next;
            if (waiter->is_thread_waiter) {
                waiter->thread_latch->count_down();
            } else {
                if (waiter->ctx) waiter->ctx->schedule(std::move(waiter->handle));
                else waiter->handle.resume();
            }
            waiter = next;
        }

        // 2. Mutex Path: Wake timed waiters
        {
            std::lock_guard lk(timed_mutex_);
            for (auto& state : timed_waiters_)
            {
                int expected = 0; // Waiting
                // Try to transition Waiting -> Signaled
                if (state->status.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
                {
                    if (state->waiter) {
                        auto h = state->waiter;
                        state->ctx->schedule([h]() { h.resume(); });
                    }
                }
            }
            timed_waiters_.clear();
        }
    }

    void reset() noexcept
    {
        void* expected = SET_PTR;
        state_.compare_exchange_strong(expected, nullptr, std::memory_order_release);
    }

    // --- The Intrusive Node for Standard Waits ---
    struct Awaiter
    {
        AsyncBaton& baton;
        ThreadContext* ctx = nullptr;
        std::coroutine_handle<> handle;
        Awaiter* next = nullptr;
        std::latch* thread_latch = nullptr;
        bool is_thread_waiter = false;

        explicit Awaiter(AsyncBaton& b, ThreadContext* c) : baton(b), ctx(c) {}
        explicit Awaiter(AsyncBaton& b, std::latch* l) : baton(b), thread_latch(l), is_thread_waiter(true) {}

        bool await_ready() const noexcept { return baton.is_set(); }
        bool await_suspend(std::coroutine_handle<> h) noexcept {
            handle = h;
            return baton.push_waiter(this);
        }
        void await_resume() noexcept {}
    };

    [[nodiscard]] Awaiter wait(ThreadContext& ctx) noexcept { return Awaiter{*this, &ctx}; }

    void wait() noexcept
    {
        if (is_set()) return;
        std::latch latch{1};
        Awaiter waiter{*this, &latch};
        if (push_waiter(&waiter)) latch.wait();
    }

    /// @brief Waits for the baton to be posted or for the timeout to expire.
    Task<bool> wait_for(ThreadContext& ctx, std::chrono::nanoseconds timeout_dur)
    {
        if (is_set()) co_return true;

        auto state = std::make_shared<RaceState>();
        state->ctx = &ctx;

        {
            std::lock_guard lk(timed_mutex_);
            if (is_set()) co_return true;
            timed_waiters_.push_back(state);
        }

        // [FIX] Capture 'self' (shared_ptr) to keep AsyncBaton alive during the timeout wait.
        std::shared_ptr<AsyncBaton> self;
        try { self = shared_from_this(); } catch (...) {}

        // If we can't get shared_from_this (stack allocated), we MUST NOT capture 'self'.
        // However, stack allocation + async timeout is dangerous if the stack object dies.
        // We will assume shared_ptr usage for safety or rely on user to keep alive.

        // We capture 'self' and use it to access mutex/waiters.
        ctx.schedule([state, timeout_dur, self, this]() -> Task<> {
            // Await the timeout
            co_await uring::timeout(state->ctx->executor(), timeout_dur);

            int expected = 0; // Waiting
            // Try to transition Waiting -> TimedOut
            if (state->status.compare_exchange_strong(expected, 2, std::memory_order_acq_rel))
            {
                // We won the race (Timeout happened).
                // We need to remove ourselves from the list.

                // CRITICAL FIX: Use 'self' if available to ensure object is alive.
                // If 'self' is null, fall back to 'this' (and hope user kept it alive).
                AsyncBaton* safe_this = self ? self.get() : this;

                {
                    std::lock_guard lk(safe_this->timed_mutex_);
                    std::erase_if(safe_this->timed_waiters_, [&](const auto& s) { return s == state; });
                }

                if (state->waiter) state->waiter.resume();
            }
        }());

        struct RaceAwaiter
        {
            std::shared_ptr<RaceState> s;
            bool await_ready() const noexcept { return false; }
            bool await_suspend(std::coroutine_handle<> h) noexcept {
                s->waiter = h;
                if (s->status.load(std::memory_order_acquire) != 0) return false;
                return true;
            }
            bool await_resume() const noexcept {
                return s->status.load(std::memory_order_acquire) == 1;
            }
        };

        co_return co_await RaceAwaiter{std::move(state)};
    }

private:
    bool push_waiter(Awaiter* waiter) noexcept
    {
        const void* signaled_state = SET_PTR;
        void* old_head = state_.load(std::memory_order_relaxed);
        while (true) {
            if (old_head == signaled_state) return false;
            waiter->next = static_cast<Awaiter*>(old_head);
            if (state_.compare_exchange_weak(old_head, waiter, std::memory_order_release, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    std::atomic<void*> state_;
    static inline void* const SET_PTR = (void*)1;
};

} // namespace uring