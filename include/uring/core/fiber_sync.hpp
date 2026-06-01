#pragma once
#include <cassert>
#include <cstddef>
#include <queue>

#include "uring/core/fiber_io.hpp"

namespace URing
{

/// Non-reentrant mutual exclusion for stackful fibers.
///
/// All fibers sharing a FiberMutex MUST run on the same IO instance.
/// Because fibers within one IO are cooperative and single-threaded, the
/// internal waiter queue and locked state need no atomic protection.
///
/// @code
/// FiberMutex mu;
///
/// io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
///     mu.lock(fio);                  // acquires or suspends
///     // ... critical section ...
///     mu.unlock(fio);                // releases or wakes next waiter
///     return {};
/// });
/// @endcode
class FiberMutex
{
    bool                      locked_{false};
    std::queue<FiberContext*>  waiters_;

public:
    /// Acquire the mutex.
    ///
    /// If the mutex is free it is acquired immediately and returns.
    /// If it is already held, the calling fiber is suspended (the OS thread
    /// keeps running other work) and resumes only once the current holder
    /// calls unlock().  Waiters are woken in FIFO order.
    void lock(FiberIO& fio)
    {
        if (!locked_)
        {
            locked_ = true;
            return;
        }
        waiters_.push(&fio.context());
        fio.suspend();
        // Ownership transferred from unlock() — we hold the lock on return.
    }

    /// Release the mutex.
    ///
    /// If no fibers are waiting, the mutex is marked free.  Otherwise,
    /// ownership is transferred directly to the oldest waiter (FIFO) without
    /// clearing the locked state — the woken fiber holds the lock on return
    /// from its lock() call.
    void unlock(FiberIO& fio)
    {
        if (waiters_.empty())
        {
            locked_ = false;
            return;
        }
        // Transfer ownership: keep locked_=true, wake the next holder.
        FiberContext* next = waiters_.front();
        waiters_.pop();
        fio.wakeup(*next);
    }

    /// Attempt to acquire the mutex without suspending.
    ///
    /// @return true if the mutex was free and has been acquired,
    ///         false if it was already held (no suspension occurs).
    [[nodiscard]] bool try_lock() noexcept
    {
        if (!locked_)
        {
            locked_ = true;
            return true;
        }
        return false;
    }

    /// @return true if the mutex is currently held by any fiber.
    [[nodiscard]] bool is_locked() const noexcept { return locked_; }

    // Non-movable: fibers store a reference to this mutex; relocating it
    // while waiters are queued would produce dangling context pointers.
    FiberMutex(const FiberMutex&)            = delete;
    FiberMutex& operator=(const FiberMutex&) = delete;
    FiberMutex(FiberMutex&&)                 = delete;
    FiberMutex& operator=(FiberMutex&&)      = delete;
};

/// RAII scoped lock for FiberMutex.
///
/// Acquires the mutex on construction (suspending if necessary) and releases
/// it unconditionally on destruction, including on early returns via FIBER_TRY.
///
/// @code
/// FiberMutex mu;
///
/// io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
///     FiberLockGuard guard{mu, fio};   // locked here
///     FIBER_TRY(auto n, fio.read_fixed(fd, buf));
///     process(buf, n);
///     return {};                       // guard releases on return
/// });
/// @endcode
class FiberLockGuard
{
    FiberMutex& mu_;
    FiberIO&    fio_;

public:
    /// Acquire @p mu, suspending this fiber if the mutex is already held.
    FiberLockGuard(FiberMutex& mu, FiberIO& fio) : mu_(mu), fio_(fio) { mu_.lock(fio_); }

    /// Release the mutex unconditionally.
    ~FiberLockGuard() { mu_.unlock(fio_); }

    FiberLockGuard(const FiberLockGuard&)            = delete;
    FiberLockGuard& operator=(const FiberLockGuard&) = delete;
    FiberLockGuard(FiberLockGuard&&)                 = delete;
    FiberLockGuard& operator=(FiberLockGuard&&)      = delete;
};

/// Counting semaphore for stackful fibers.
///
/// All fibers sharing a FiberSemaphore MUST run on the same IO instance.
/// A FiberMutex is equivalent to a FiberSemaphore with an initial count of 1,
/// but with ownership-transfer semantics.  Use FiberSemaphore for signalling
/// (e.g. one fiber waiting for another to complete a step).
///
/// @code
/// FiberSemaphore ready{0};  // starts blocked
///
/// io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
///     // producer: do work, then signal consumer
///     FIBER_TRY_VOID(fio.fsync(fd));
///     ready.post(fio);
///     return {};
/// });
///
/// io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
///     ready.wait(fio);  // suspends until producer posts
///     // ... consume result ...
///     return {};
/// });
/// @endcode
class FiberSemaphore
{
    std::ptrdiff_t            count_;
    std::queue<FiberContext*>  waiters_;

public:
    /// Construct a semaphore with the given initial count.
    ///
    /// @param initial  Starting count; must be >= 0.  A value of 0 means the
    ///                 first wait() call will suspend.  A value of N lets N
    ///                 fibers wait() without blocking before the (N+1)th parks.
    explicit FiberSemaphore(const std::ptrdiff_t initial = 0) noexcept : count_(initial)
    {
        assert(initial >= 0);
    }

    /// Decrement the count and return, or suspend if the count is zero.
    ///
    /// The fiber resumes (and the count stays at zero) once another fiber
    /// calls post().  Waiters are woken in FIFO order.
    void wait(FiberIO& fio)
    {
        if (count_ > 0)
        {
            --count_;
            return;
        }
        waiters_.push(&fio.context());
        fio.suspend();
    }

    /// Increment the count, or wake the oldest waiting fiber if count is zero.
    ///
    /// If a fiber is parked in wait(), it is re-enqueued for the next tick and
    /// the count is left at zero (the slot is consumed by the waiter).
    /// If no fiber is waiting, the count is incremented for a future wait().
    void post(FiberIO& fio)
    {
        if (waiters_.empty())
        {
            ++count_;
            return;
        }
        FiberContext* next = waiters_.front();
        waiters_.pop();
        fio.wakeup(*next);
    }

    /// @return The current count (number of available permits).
    [[nodiscard]] std::ptrdiff_t value() const noexcept { return count_; }

    // Non-movable: same rationale as FiberMutex.
    FiberSemaphore(const FiberSemaphore&)            = delete;
    FiberSemaphore& operator=(const FiberSemaphore&) = delete;
    FiberSemaphore(FiberSemaphore&&)                 = delete;
    FiberSemaphore& operator=(FiberSemaphore&&)      = delete;
};

/// Bounded FIFO channel for passing values between stackful fibers.
///
/// @tparam T  Value type.  Must be movable.
/// @tparam N  Buffer capacity (must be > 0).  A full buffer causes the sending
///            fiber to suspend; an empty buffer causes the receiving fiber to
///            suspend.
///
/// All fibers sharing a FiberChannel MUST run on the same IO instance.
/// Blocked senders and receivers are woken in FIFO order.
///
/// @code
/// FiberChannel<std::string, 8> ch;
///
/// // Producer fiber
/// io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
///     for (auto& line : lines)
///         ch.send(fio, line);        // suspends if buffer full
///     return {};
/// });
///
/// // Consumer fiber — use a known count, not empty(), to decide when to stop.
/// // empty() reflects only the buffer; it does not account for suspended senders.
/// io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
///     for (std::size_t i = 0; i < lines.size(); ++i)
///         process(ch.recv(fio));     // suspends if buffer empty
///     return {};
/// });
/// @endcode
template <typename T, std::size_t N>
class FiberChannel
{
    static_assert(N > 0, "FiberChannel capacity must be > 0");

    struct PendingSend
    {
        T             value;
        FiberContext* sender;
    };

    std::queue<T>             buffer_;
    std::queue<PendingSend>   pending_sends_;
    std::queue<FiberContext*> pending_recvs_;

public:
    /// Send @p value through the channel.
    ///
    /// - If a receiver is already waiting in recv(), the value is placed in the
    ///   buffer and the receiver is woken immediately.
    /// - If the buffer has room, the value is enqueued and the call returns.
    /// - If the buffer is full, this fiber suspends until a receiver drains a slot.
    void send(FiberIO& fio, T value)
    {
        if (!pending_recvs_.empty())
        {
            // A receiver is already waiting — push the value so recv() finds
            // it when it resumes, then wake the receiver.
            FiberContext* recv_ctx = pending_recvs_.front();
            pending_recvs_.pop();
            buffer_.push(std::move(value));
            fio.wakeup(*recv_ctx);
            return;
        }
        if (buffer_.size() < N)
        {
            buffer_.push(std::move(value));
            return;
        }
        // Buffer full: park until a receiver frees a slot.
        pending_sends_.push({std::move(value), &fio.context()});
        fio.suspend();
    }

    /// Receive the next value from the channel.
    ///
    /// - If the buffer is non-empty, the oldest value is returned immediately.
    /// - If the buffer is empty, this fiber suspends until a sender delivers a value.
    ///
    /// If a sender was blocked waiting for space, it is woken after the receive
    /// drains a slot and its value is placed into the buffer.
    ///
    /// @return The oldest buffered value, moved out of the channel.
    T recv(FiberIO& fio)
    {
        if (buffer_.empty())
        {
            // No data yet: park until a sender delivers.
            pending_recvs_.push(&fio.context());
            fio.suspend();
            // After resumption the sender has already pushed to buffer_.
        }
        T val = std::move(buffer_.front());
        buffer_.pop();
        // Unpark a pending sender if any, transferring its value into the buffer.
        if (!pending_sends_.empty())
        {
            PendingSend ps = std::move(pending_sends_.front());
            pending_sends_.pop();
            buffer_.push(std::move(ps.value));
            fio.wakeup(*ps.sender);
        }
        return val;
    }

    /// @return Number of values currently buffered (does not include values held
    ///         by suspended senders).
    [[nodiscard]] std::size_t size()  const noexcept { return buffer_.size(); }

    /// @return true if no values are currently buffered.
    [[nodiscard]] bool        empty() const noexcept { return buffer_.empty(); }

    /// @return true if the buffer has reached capacity N (further sends will suspend).
    [[nodiscard]] bool        full()  const noexcept { return buffer_.size() >= N; }

    // Non-movable: same rationale as FiberMutex.
    FiberChannel(const FiberChannel&)            = delete;
    FiberChannel& operator=(const FiberChannel&) = delete;
    FiberChannel(FiberChannel&&)                 = delete;
    FiberChannel& operator=(FiberChannel&&)      = delete;
};

}  // namespace URing
