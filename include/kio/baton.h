#pragma once

#include "detail/ready_ring.h"
#include "kio/context.h"

#include <coroutine>

namespace kio
{
// One-shot async event, kio's folly::coro::Baton: co_await baton parks
// until post(); post() is STICKY (once posted, always posted) and
// BROADCAST (all current waiters wake); late waiters complete inline.
// No payload, no reset (a resettable flag is a different primitive --
// reset+wait races are where these designs rot), no cancellation.
//
// Loop-thread only (the kio contract): no atomics, no locks. Wakes go
// through Context::post -- Phase 1 of the run loop -- never inline on
// the poster's stack. Parked handles cannot dangle: kio Tasks
// CHECK-abort when destroyed while suspended.
//
// No reserved-wake accounting is needed here (contrast Semaphore): a
// posted Baton's ready_ is sticky, so an arrival during the wake drain
// sees await_ready() == true and never parks. Accounting exists only
// when a released RESOURCE can be stolen.
//
// Quiescence: await_suspend notifies the Context's park counter, the
// woken await_resume consumes it, and run()'s exit CHECK turns a
// never-posted baton at quiescence into a fail-fast abort (its poster
// would have to be runnable, in flight, or itself parked -- a cycle).
class Baton
{
public:
    explicit Baton(io::Context& io) noexcept : io_(io) {}

    Baton(const Baton&) = delete;
    Baton& operator=(const Baton&) = delete;

    /// Latch and wake every parked waiter (Context::post -> Phase 1).
    /// Idempotent: a second post() when already ready is a no-op.
    void post() noexcept
    {
        ready_ = true;
        std::coroutine_handle<> h;
        while (waiters_.pop_batch(&h, 1) == 1)
        {
            io_.post(h);
        }
    }

    [[nodiscard]] bool try_wait() const noexcept { return ready_; }

    struct WaitAwaiter
    {
        Baton& baton;
        bool parked_ = false;  // guards note_unpark on the fast path

        explicit WaitAwaiter(Baton& b) noexcept : baton(b) {}

        WaitAwaiter(const WaitAwaiter&) = delete;
        WaitAwaiter& operator=(const WaitAwaiter&) = delete;

        bool await_ready() const noexcept { return baton.ready_; }

        void await_suspend(std::coroutine_handle<> h) noexcept
        {
            baton.io_.note_park();
            parked_ = true;
            baton.waiters_.push(h);
        }

        void await_resume() noexcept
        {
            if (parked_)
            {
                baton.io_.note_unpark();
            }
        }
    };

    [[nodiscard]] WaitAwaiter wait() noexcept { return WaitAwaiter{*this}; }

private:
    io::Context& io_;
    bool ready_ = false;
    detail::ReadyRing waiters_{};
};
}  // namespace kio
