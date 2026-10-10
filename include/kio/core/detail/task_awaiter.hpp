#pragma once

// ============================================================================
// kio::detail::TaskAwaiter -- the co_await bridge for Task<T>
// ============================================================================
//
// This is the *consumer* half of the promise protocol documented in
// promise_base.hpp. Read that file's THE ONE RULE first: it explains frame
// ownership. Everything here exists to serve that rule without touching the
// frame.
//
// WHAT THIS TYPE IS
// ----------------
//
// A trivially-copyable view over another coroutine's frame:
//
//     TaskAwaiter{ h }   ->  { h, no state of its own }
//
// It is returned by value from Task<T>::operator co_await() and therefore
// lives as a temporary inside the *awaiting* coroutine's frame. It is NOT
// stored in the frame it points at -- keeping these two separate is what makes
// the "hand control over" step below well-defined: the awaiting frame stays
// suspended while the awaited one runs, and neither can move in memory.
//
// THE THREE-STEP HANDOVER
// -----------------------
//
//   awaiting coroutine          awaited coroutine (Task T)
//   --------------------        --------------------------------
//   co_await t
//     await_ready()
//        false  ------------->   body has not run yet (initial_suspend
//                                    is suspend_always). Stay awake.
//     await_suspend(caller)
//        promise.continuation
//              = caller  ------->  written ONCE, before the body ever runs.
//        return t_handle  ------->  control transfers INTO the awaited frame.
//                                    Caller is parked; only `continuation`
//                                    remembers it exists.
//              . . . time passes . . .
//                                 co_return v
//                                 final_suspend ->
//                                   FinalAwaitable sees continuation != null
//                                   and returns it
//       <------------------------  control transfers BACK here
//     await_resume()
//        move Result out
//
// INVARIANT: exactly one write to `continuation` per frame. It happens in
// await_suspend, which runs before the body starts, so the body and the
// final_suspend that reads it can never race.
// ============================================================================

#include <cassert>

#include "promise_base.hpp"

namespace kio::detail
{
// ----------------------------------------------------------------------------
// TaskAwaiter<T> -- awaits a Task<T> and yields its Result<T>
// ----------------------------------------------------------------------------
//
// INVARIANT: `handle` may be null. It is null when the Task was moved from or
// released (Task::release() hands the frame to the scheduler and leaves the
// Task empty). Every method below is written to tolerate that rather than
// assume it away -- a moved-from Task is a legal, observable state, not UB.
//
// Not copyable-from-nothing in particular: the awaiter is a value, so copying
// it is free and harmless; the frame is shared, never duplicated.
template <typename T>
struct TaskAwaiter
{
    using Handle = std::coroutine_handle<TaskPromise<T>>;
    Handle handle;

    // A moved-from or released Task has a null handle. Calling done() on it is
    // undefined behaviour, so co_await must not suspend into a dead frame.
    //
    // Both halves matter:
    //   !handle        -> nothing to await; report immediately (await_resume
    //                    turns the null into operation_canceled).
    //   handle.done()  -> the coroutine already finished before we got here.
    //                    Result is sitting in the frame; do NOT re-suspend,
    //                    because final_suspend has already run and would
    //                    transfer control to a stale/absent continuation.
    bool await_ready() const noexcept { return !handle || handle.done(); }

    // Publishing the continuation and transferring in.
    //
    // The store must COMPLETE before we return `handle`. From that point the
    // awaited coroutine is running, and whatever eventually resumes it lands in
    // FinalAwaitable, which reads this field. Publishing it late would hand
    // control to nobody and leak the frame. The store is plain (seq_cst), which
    // is stronger than a release store would need, and free on x86-64.
    //
    // Returning `handle` (rather than void) means "suspend me and resume that
    // one". The caller is now parked inside this function's caller; the only
    // trace of it is this promise field.
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
    {
        handle.promise().continuation = caller;
        return handle;
    }

    // Runs back on the awaiting coroutine after control is transferred to us
    // from FinalAwaitable. The awaited frame is still alive here -- nobody has
    // destroyed it -- which is the entire reason the transfer is safe.
    //
    // Single-consume contract: the Result is MOVED out, so a second read would
    // hand back a moved-from T. `result_taken` makes that a diagnosed bug in
    // debug builds and a defined operation_canceled in release builds. Both
    // branches below are deliberately present: the assert alone would vanish
    // under NDEBUG and turn a logic error into UB.
    Result<T> await_resume() noexcept
    {
        if (!handle) [[unlikely]]
        {
            return kio::Error::fail_errc(std::errc::operation_canceled);
        }

        auto& p = handle.promise();
        assert(!p.result_taken && "Task awaited twice; the Result is single-consume");

        // Defined fallback for release builds, where the assert is compiled out.
        //
        // !p.result.has_value() means the body returned without ever calling
        // co_return. That is UB per the standard, but it is recoverable in
        // practice and far more useful than dereferencing an empty optional.
        if (p.result_taken || !p.result.has_value()) [[unlikely]]
        {
            return kio::Error::fail_errc(std::errc::operation_canceled);
        }

        // Set the guard BEFORE the move, so a re-entrant read cannot slip
        // between the check and the move.
        p.result_taken = true;
        return std::move(*p.result);
    }
};
}  // namespace kio::detail