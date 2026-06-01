#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <list>
#include <optional>
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
///     FIBER_TRY_VOID(mu.lock(fio));  // acquires or suspends
///     // ... critical section ...
///     mu.unlock(fio);                // releases or wakes next waiter
///     return {};
/// });
/// @endcode
class FiberMutex
{
    struct Waiter
    {
        FiberContext* ctx;
        bool          woken{false};
    };

    bool                      locked_{false};
    std::list<Waiter>          waiters_;

public:
    FiberMutex() = default;

    /// Acquire the mutex.
    ///
    /// If the mutex is free it is acquired immediately and returns.
    /// If it is already held, the calling fiber is suspended (the OS thread
    /// keeps running other work) and resumes only once the current holder
    /// calls unlock().  Waiters are woken in FIFO order.
    Result<void> lock(FiberIO& fio)
    {
        if (!locked_)
        {
            locked_ = true;
            return {};
        }
        auto it = waiters_.insert(waiters_.end(), Waiter{&fio.context()});
        auto res = fio.suspend();
        waiters_.erase(it);
        if (!res) [[unlikely]]
        {
            return std::unexpected(res.error());
        }
        // Ownership transferred from unlock() — we hold the lock on return.
        return {};
    }

    /// Release the mutex.
    ///
    /// If no fibers are waiting, the mutex is marked free.  Otherwise,
    /// ownership is transferred directly to the oldest waiter (FIFO) without
    /// clearing the locked state — the woken fiber holds the lock on return
    /// from its lock() call.
    void unlock(FiberIO& fio)
    {
        auto next = std::ranges::find_if(waiters_.begin(), waiters_.end(), [](const Waiter& waiter)
        {
            return !waiter.woken;
        });
        if (next == waiters_.end())
        {
            locked_ = false;
            return;
        }
        // Transfer ownership: keep locked_=true, wake the next holder.
        next->woken = true;
        fio.wakeup(*next->ctx);
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
///     FIBER_TRY_VOID(guard.result());
///     FIBER_TRY(auto n, fio.read_fixed(fd, buf));
///     process(buf, n);
///     return {};                       // guard releases on return
/// });
/// @endcode
class FiberLockGuard
{
    FiberMutex& mu_;
    FiberIO&    fio_;
    bool        acquired_{false};
    Result<void> result_{};

public:
    /// Acquire @p mu, suspending this fiber if the mutex is already held.
    FiberLockGuard(FiberMutex& mu, FiberIO& fio) : mu_(mu), fio_(fio)
    {
        result_ = mu_.lock(fio_);
        acquired_ = result_.has_value();
    }

    /// Release the mutex unconditionally.
    ~FiberLockGuard()
    {
        if (acquired_)
            mu_.unlock(fio_);
    }

    [[nodiscard]] Result<void> result() const
    {
        if (!result_)
            return std::unexpected(result_.error());
        return {};
    }

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
///     FIBER_TRY_VOID(ready.wait(fio));  // suspends until producer posts
///     // ... consume result ...
///     return {};
/// });
/// @endcode
class FiberSemaphore
{
    struct Waiter
    {
        FiberContext* ctx;
        bool          woken{false};
    };

    std::ptrdiff_t            count_;
    std::list<Waiter>          waiters_;

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
    Result<void> wait(FiberIO& fio)
    {
        if (count_ > 0)
        {
            --count_;
            return {};
        }
        auto it = waiters_.insert(waiters_.end(), Waiter{&fio.context()});
        auto res = fio.suspend();
        waiters_.erase(it);
        if (!res) [[unlikely]]
        {
            return std::unexpected(res.error());
        }
        return {};
    }

    /// Increment the count, or wake the oldest waiting fiber if count is zero.
    ///
    /// If a fiber is parked in wait(), it is re-enqueued for the next tick and
    /// the count is left at zero (the slot is consumed by the waiter).
    /// If no fiber is waiting, the count is incremented for a future wait().
    void post(FiberIO& fio)
    {
        auto next = std::ranges::find_if(waiters_.begin(), waiters_.end(), [](const Waiter& waiter)
        {
            return !waiter.woken;
        });
        if (next == waiters_.end())
        {
            ++count_;
            return;
        }
        next->woken = true;
        fio.wakeup(*next->ctx);
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
///         FIBER_TRY_VOID(ch.send(fio, line));        // suspends if buffer full
///     return {};
/// });
///
/// // Consumer fiber — use a known count, not empty(), to decide when to stop.
/// // empty() reflects only the buffer; it does not account for suspended senders.
/// io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
///     for (std::size_t i = 0; i < lines.size(); ++i)
///     {
///         FIBER_TRY(auto value, ch.recv(fio));       // suspends if buffer empty
///         process(value);
///     }
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
        FiberContext* sender{};
        bool          woken{false};
    };

    struct PendingRecv
    {
        FiberContext*     receiver{};
        std::optional<T>* slot;
        bool              woken{false};
    };

    std::queue<T>           buffer_;
    std::list<PendingSend>  pending_sends_;
    std::list<PendingRecv>  pending_recvs_;

public:
    FiberChannel() = default;

    /// Send @p value through the channel.
    ///
    /// - If a receiver is already waiting in recv(), the value is placed in the
    ///   buffer and the receiver is woken immediately.
    /// - If the buffer has room, the value is enqueued and the call returns.
    /// - If the buffer is full, this fiber suspends until a receiver drains a slot.
    Result<void> send(FiberIO& fio, T value)
    {
        auto recv_it = std::find_if(pending_recvs_.begin(), pending_recvs_.end(), [](const PendingRecv& recv)
        {
            return !recv.woken;
        });
        if (recv_it != pending_recvs_.end())
        {
            // A receiver is already waiting — deliver directly to its stack
            // slot, keeping the bounded buffer at or below capacity N.
            recv_it->slot->emplace(std::move(value));
            recv_it->woken = true;
            fio.wakeup(*recv_it->receiver);
            return {};
        }
        if (buffer_.size() < N)
        {
            buffer_.push(std::move(value));
            return {};
        }
        // Buffer full: park until a receiver frees a slot.
        auto it = pending_sends_.insert(pending_sends_.end(), PendingSend{std::move(value), &fio.context()});
        auto res = fio.suspend();
        pending_sends_.erase(it);
        if (!res) [[unlikely]]
        {
            return std::unexpected(res.error());
        }
        return {};
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
    Result<T> recv(FiberIO& fio)
    {
        if (buffer_.empty())
        {
            // No data yet: park until a sender delivers.
            std::optional<T> delivered;
            auto it = pending_recvs_.insert(pending_recvs_.end(), PendingRecv{&fio.context(), &delivered});
            auto res = fio.suspend();
            pending_recvs_.erase(it);
            if (!res) [[unlikely]]
            {
                return std::unexpected(res.error());
            }
            if (!delivered) [[unlikely]]
                return error_from_errno(ECANCELED);
            return std::move(*delivered);
        }
        T val = std::move(buffer_.front());
        buffer_.pop();
        // Unpark a pending sender if any, transferring its value into the buffer.
        auto send_it = std::find_if(pending_sends_.begin(), pending_sends_.end(), [](const PendingSend& send)
        {
            return !send.woken;
        });
        if (send_it != pending_sends_.end())
        {
            auto recv_it = std::find_if(pending_recvs_.begin(), pending_recvs_.end(), [](const PendingRecv& recv)
            {
                return !recv.woken;
            });
            if (recv_it != pending_recvs_.end())
            {
                recv_it->slot->emplace(std::move(send_it->value));
                recv_it->woken = true;
                fio.wakeup(*recv_it->receiver);
            }
            else
            {
                buffer_.push(std::move(send_it->value));
            }
            send_it->woken = true;
            fio.wakeup(*send_it->sender);
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
