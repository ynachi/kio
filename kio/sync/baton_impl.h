#ifndef KIO_BATON_IMPL_H
#define KIO_BATON_IMPL_H

#include "kio/core/worker.h"
#include "kio/sync/baton.h"

namespace kio::sync
{

inline void AsyncBaton::Post() noexcept
{
    void* const signaled_state = this;

    // Optimization: Double-checked locking pattern (without the lock)
    if (state_.load(std::memory_order_acquire) == signaled_state)
    {
        return;
    }

    // Atomically swap to signaled state
    if (void* old_state = state_.exchange(signaled_state, std::memory_order_acq_rel);
        old_state != signaled_state && old_state != nullptr)
    {
        // Walk the linked list and resume everyone
        const auto* awaiter = static_cast<Awaiter*>(old_state);
        while (awaiter != nullptr)
        {
            const auto* next = awaiter->next;
            // Resume on the awaiter's original Worker (thread affinity)
            awaiter->worker->Post(awaiter->handle);
            awaiter = next;
        }
    }

    // Handle timed waiters
    TimedAwaiter* timed_waiters = TakeTimedWaiters();
    while (timed_waiters != nullptr)
    {
        TimedAwaiter* next = timed_waiters->next;
        // Try to claim the completion (it might have timed out concurrently)
        if (timed_waiters->TryComplete(TimedAwaiter::State::kSignaled))
        {
            timed_waiters->worker->Post(timed_waiters->handle);
        }
        timed_waiters = next;
    }
}

inline bool AsyncBaton::TimedAwaiter::await_suspend(std::coroutine_handle<> coro_handle)
{
    handle = coro_handle;

    if (timeout <= std::chrono::nanoseconds::zero())
    {
        state.store(State::kTimedOut, std::memory_order_release);
        return false;
    }

    if (!baton.EnqueueTimed(this))
    {
        return false;
    }

    // Spawn a task using Worker::AsyncSleep
    // This simplifies logic: no manual io_uring prep needed.
    timeout_task.emplace(
        [this]() -> Task<void>
        {
            // Wait for the worker to sleep. We ignore the result (void/error)
            // because any completion implies time has passed or cancelled.
            (void)co_await worker->AsyncSleep(timeout);

            // Try to mark as TimedOut. If this fails, it means we were Signaled
            // concurrently, so we do nothing.
            if (!TryComplete(State::kTimedOut))
            {
                co_return;
            }

            // If we successfully timed out, remove ourselves and resume.
            baton.RemoveTimed(this);
            worker->Post(handle);
        }());

    timeout_task->h_.resume();
    return true;
}

}  // namespace kio::sync

#endif  // KIO_BATON_IMPL_H