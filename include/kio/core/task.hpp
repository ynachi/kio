#pragma once

// ============================================================================
// kio::Task<T> -- the primary coroutine type
// ============================================================================
//
// A lazy, move-only, non-copyable handle to a suspended coroutine frame that
// produces a Result<T>. This file is the OWNER side of the protocol described
// in detail/promise_base.hpp; detail/task_awaiter.hpp is the consumer side.
//
// WHO OWNS THE FRAME
// -----------------
//
// Exactly one of two parties, and Task<T> is the switch between them:
//
//   Task<T> t = do_work();     frame exists, body has NOT run
//   Task<T> t2 = std::move(t); // ownership transferred; t is now null
//   ~Task<T>()                 frame destroyed (if t still holds it)
//   Task<T>::release()         ownership handed to the SCHEDULER; promise.
//                              detached = true; the frame destroys itself at
//                              final_suspend because nobody owns it any more
//
// INVARIANT (frame liveness): handle_ == nullptr implies the frame is NOT
// this object's responsibility. Every destructor, get() and release() below is
// written against that implication. handle_ != nullptr implies the frame is
// alive, even if it has already run to completion -- a completed-but-unclaimed
// frame is still ours to destroy, and destroying it is how we reclaim it.
//
// INVARIANT (thread affinity): a Task<T> is NOT thread-safe and does not need
// to be. It is created and consumed on one thread; crossing threads happens by
// calling IO::schedule(), which moves the frame onto that worker's MPSC queue.
// Nothing inside this type touches memory another thread can reach
// concurrently -- see TaskPromiseBase::next, which is the one atomic member and
// belongs to the queue, not to Task.
//
// WHY [[nodiscard]]
// ----------------
// Every exit path is either a co_await site or a schedule() site, both of which
// explicitly take the handle. If you construct a Task and do neither, the
// coroutine never runs and its result is silently dropped. The attribute makes
// that a compile error instead of a runtime surprise.
//
// ============================================================================

#include <cassert>
#include <coroutine>
#include <utility>

#include "detail/promise_base.hpp"
#include "detail/task_awaiter.hpp"

namespace kio
{
// ============================================================================
// Task<T> — The Primary Coroutine Type
// ============================================================================
//
// Move-only. The copy operations are deleted rather than merely unused: a
// copied Task would mean two owners of one frame, which is exactly the
// use-after-free that promise_base.hpp's THE ONE RULE exists to prevent.
template <typename T>
struct [[nodiscard]] Task
{
    using promise_type = detail::TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    // The one and only frame handle. Null == we do not own a frame (moved from
    // or released). See the file-level invariants above.
    Handle handle_;

    // Called by the compiler via get_return_object() at coroutine creation.
    explicit Task(Handle h) noexcept : handle_(h) {}

    // Move ctor steals ownership and leaves the source null (std::exchange).
    // The nulling is what makes the self-move and double-move cases safe.
    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}

    // Move assignment must destroy whatever we were holding FIRST: overwriting
    // handle_ without destroying it would leak a running coroutine's frame.
    //
    // Self-assignment is checked because std::exchange on `o == this` would
    // null out handle_ and then store the null we just took.
    Task& operator=(Task&& o) noexcept
    {
        if (this != &o)
        {
            if (handle_)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(o.handle_, {});
        }
        return *this;
    }

    // Copying would duplicate ownership of a single frame. Deleted, not defaulted.
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // INVARIANT: destroying here is always correct, and always the last touch.
    // If the frame already ran to completion, final_suspend parked it (nobody
    // was waiting and detached == false), and this is the reclaim. If it is
    // still suspended mid-body, we are cancelling it -- legal, because a frame
    // we own cannot have an outstanding io_uring operation pointing at it
    // (see IO's drain rules in core/io.h).
    ~Task()
    {
        if (handle_)
        {
            handle_.destroy();
        }
    }

    // ------------------------------------------------------------------------
    // get() -- blocking-style extraction for coroutines that were never awaited
    // ------------------------------------------------------------------------
    //
    // Legal ONLY on a frame that has finished and that nobody else has claimed.
    // This is the "fire the coroutine, then come back for the value" path used
    // by tests and by any code that hands a Task to something that is not a
    // coroutine. If you are inside a coroutine, prefer co_await: it is the same
    // single-consume Result but goes through TaskAwaiter, which also wires up
    // the continuation.
    //
    // Returns Result<T>, never throws and never blocks.
    Result<T> get() noexcept
    {
        assert(handle_ && "Task::get() on a moved-from or released Task");
        assert(handle_.done() && "Task::get() before the coroutine completed");

        // Asserts vanish under NDEBUG, so keep a defined fallback rather than
        // dereferencing an empty optional. This also covers the genuinely
        // reachable case: a Task whose body is still suspended.
        if (!handle_ || !handle_.done()) [[unlikely]]
        {
            return kio::Error::fail_errc(std::errc::operation_canceled);
        }

        assert(!handle_.promise().result_taken && "Task::get() called twice; the Result is single-consume");

        // Same double-read guard as TaskAwaiter::await_resume. An engaged
        // optional holding an error is a legitimate result and is returned as
        // such; only a nullopt (body never co_return'd) becomes canceled here.
        if (handle_.promise().result_taken || !handle_.promise().result.has_value()) [[unlikely]]
        {
            return kio::Error::fail_errc(std::errc::operation_canceled);
        }

        handle_.promise().result_taken = true;
        return std::move(*handle_.promise().result);
    }

    // -----------------------------------------------------------------------
    // release() -- hand the frame to the scheduler
    // -----------------------------------------------------------------------
    //
    // Transfers ownership of the coroutine frame to the scheduler.
    // The Task object becomes empty and the frame will self-destruct on completion.
    //
    // This is the ONLY sanctioned way to get an unstarted coroutine running on
    // another thread:
    //
    //     Task<...> t = handle_conn();
    //     io.schedule(std::move(t));   // schedule() calls release() internally
    //
    // Setting detached = true here is what later lets FinalAwaitable decide it
    // may self-destroy: from this point nothing outside the scheduler holds the
    // handle, so parking forever would leak and destroying unconditionally
    // would be correct. Before this call the flag is false and destroying
    // unconditionally was the bug that produced the ASan report recorded in
    // promise_base.hpp.
    Handle release()
    {
        assert(handle_ && "Task::release() on a moved-from or already-released Task");
        auto h = std::exchange(handle_, {});
        h.promise().detached = true;
        return h;
    }

    // True only if we still own a frame AND that frame has finished. A
    // moved-from Task returns false rather than dereferencing null -- callers
    // poll this in a loop, so it must be safe to ask at any time.
    bool done() const noexcept { return handle_ && handle_.done(); }

    // -----------------------------------------------------------------------
    // co_await support — Returns Result<T> to the caller.
    // -----------------------------------------------------------------------
    //
    // Returns a TaskAwaiter BY VALUE. The awaiter holds a copy of handle_ but
    // owns nothing; the Task itself may be destroyed while the await is in
    // flight, because the awaiting coroutine's frame holds the copy the
    // awaiter is using. That is why this does not return a reference and why
    // detaching the Task before co_await'ing it is not required.
    auto operator co_await() noexcept { return detail::TaskAwaiter{handle_}; }
};

// Out-of-line get_return_object definitions. They live here rather than in
// promise_base.hpp because Task<T> is incomplete at the point the promise type
// is instantiated; this is the standard work-around, and doing it inline would
// be a recursive-instantiation error.
//
// get_return_object() runs exactly once, immediately after the frame is
// allocated and before initial_suspend() parks it -- so the Task handed back
// to the caller is fully formed but not yet running.
template <typename T>
Task<T> detail::TaskPromise<T>::get_return_object() noexcept
{
    return Task<T>{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

inline Task<void> detail::TaskPromise<void>::get_return_object() noexcept
{
    return Task{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

} // namespace kio