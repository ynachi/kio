#pragma once

#include "absl/log/check.h"
#include "detail/ready_ring.h"
#include "kio/context.h"

#include <coroutine>
#include <cstddef>

namespace kio
{
// Counting async semaphore, wake-one FIFO -- the resource counterpart of
// Baton (an event). Same loop-thread contract: no atomics, wakes via
// Context::post -> Phase 1, parked handles cannot dangle.
//
// Reserved-wake protocol (mandatory here, unlike Baton): a released
// PERMIT can be stolen by a newcomer between wake and resume, so
// release() counts the woken waiter in reserved_ and try_acquire()
// serves only the SURPLUS (count_ > reserved_). The woken waiter
// consumes its reservation and takes the permit it is backed by.
// Invariants: count_ >= reserved_ always; when waiters exist,
// count_ == reserved_ (no surplus, or they would not have parked).
// No close()/cancellation in v1: a parked waiter whose releaser is
// neither runnable nor in flight is a deadlock, caught by run()'s exit
// CHECK via the park counter.
class Semaphore
{
public:
    explicit Semaphore(io::Context& io, const std::size_t initial) noexcept : io_(io), count_(initial) {}

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    /// Non-blocking acquire. Newcomers may only take surplus permits.
    [[nodiscard]] bool try_acquire() noexcept
    {
        if (count_ > reserved_)
        {
            --count_;
            return true;
        }
        return false;
    }

    /// Release one permit; wake the oldest waiter if one is parked.
    /// May be called when count_ was at its initial maximum (the count
    /// is a floor, not a ceiling -- same latitude as POSIX semaphores).
    void release() noexcept
    {
        ++count_;
        if (!waiters_.empty())
        {
            std::coroutine_handle<> h;
            if (waiters_.pop_batch(&h, 1) == 1)
            {
                // The surplus permit is spoken for until the waiter
                // resumes and consumes the reservation.
                ++reserved_;
                io_.post(h);
            }
        }
    }

    struct AcquireAwaiter
    {
        Semaphore& sem;
        bool fast_ = false;  // set only on the await_ready fast path

        explicit AcquireAwaiter(Semaphore& s) noexcept : sem(s) {}

        AcquireAwaiter(const AcquireAwaiter&) = delete;
        AcquireAwaiter& operator=(const AcquireAwaiter&) = delete;

        bool await_ready() noexcept
        {
            fast_ = sem.try_acquire();
            return fast_;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept
        {
            sem.io_.note_park();
            sem.waiters_.push(h);
        }

        void await_resume() noexcept
        {
            if (fast_)
            {
                return;
            }
            sem.io_.note_unpark();
            // Consume the reservation release() made for us, then take
            // the permit it is backed by. Both CHECKs are invariants.
            CHECK(sem.reserved_ > 0) << "woken semaphore waiter without a reservation";
            --sem.reserved_;
            CHECK(sem.count_ > 0) << "woken semaphore waiter without a permit";
            --sem.count_;
        }
    };

    [[nodiscard]] AcquireAwaiter acquire() noexcept { return AcquireAwaiter{*this}; }

    /// Free permits (including those reserved for woken waiters).
    [[nodiscard]] std::size_t available() const noexcept { return count_; }

    [[nodiscard]] std::size_t waiting() const noexcept { return waiters_.size(); }

private:
    friend struct AcquireAwaiter;

    io::Context& io_;
    std::size_t count_ = 0;
    std::size_t reserved_ = 0;
    detail::ReadyRing waiters_{};
};
}  // namespace kio
