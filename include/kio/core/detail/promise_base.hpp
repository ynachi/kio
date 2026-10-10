#pragma once

// ============================================================================
// kio::detail -- coroutine promise plumbing
// ============================================================================
//
// The smallest and least optional part of kio. Nothing here talks to the
// kernel; it defines who owns a coroutine frame and when it may die.
//
// THE ONE RULE
// ------------
//   A coroutine frame is owned by exactly one of:
//     (a) a live Task<T> on the caller's stack, or
//     (b) the scheduler, once Task<T>::release() has handed it over.
//
// Which one is recorded in TaskPromiseBase::detached. Every destroy() in the
// codebase is gated on that flag. Getting this wrong is not a leak, it is a
// use-after-free: one path frees the frame while the other still holds
// handle_. See FinalAwaitable below for the case that bit us.
//
// FRAME LIFECYCLE
// ---------------
//
//   Task<T> t = do_work();      do_work() is NOT running yet -- the promise's
//        |                      initial_suspend() is suspend_always, so the
//        |                      body has not executed a single statement.
//        |
//        |  co_await t          TaskAwaiter::await_suspend sets continuation
//        v                      = the awaiting coroutine, then transfers in.
//   [ RUNNING ] ------------------+
//        |                      |
//        | co_return v          | co_return / an exception
//        v                      v
//   [ AT FINAL_SUSPEND ] --> continuation != null --> transfer back to awaiter,
//        |                                  which runs await_resume() and
//        |                                  moves the Result out.
//        |
//        | continuation == nullptr  (nobody is waiting on us)
//        |
//        +--> detached == false --> SUSPEND FOREVER.
//        |                        The owner must destroy the frame:
//        |                          * Task<T>::~Task()  (dropped handle)
//        |                          * nothing, if co_await'd -- the frame
//        |                            is then reachable only via the awaiter,
//        |                            which is why await_resume() must run
//        |                            before the frame is ever freed.
//        |
//        +--> detached == true  --> log_discarded_result(); h.destroy();
//                                  ownership already went to the scheduler
//                                  (Task<T>::release()).
//
// Note the asymmetry that is easy to misread: `noop_coroutine()` appears at
// final_suspend in BOTH the "no continuation" and the "continuation" paths,
// but with opposite meanings. As a RETURN value from await_suspend it always
// means "suspend and resume nobody" -- the handle returned to the caller
// decides what happens next, not the noop itself. So:
//     return std::noop_coroutine();  -> park here forever
//     return cont;                   -> transfer control to `cont` now
// Both paths suspend; they differ only in whether something else will ever
// resume this frame.
//
// ============================================================================

#include <atomic>
#include <coroutine>
#include <exception>
#include <optional>

#include "kio/error.hpp"
#include "kio/logger.hpp"

namespace kio
{
    // Forward declarations
    template <typename T>
    struct Task;
} // namespace kio

namespace kio::detail
{
    // -----------------------------------------------------------------------
    // FinalAwaitable -- decides who gets to keep the frame
    // -----------------------------------------------------------------------
    //
    // final_suspend() hands control here at the instant the coroutine body
    // returns. The frame is still alive and must either be transferred to
    // somebody or destroyed here; leaking it is never correct.
    //
    // Decision table (all four combinations are handled):
    //
    //   continuation  detached   action
    //   ------------  --------   ------------------------------------------
    //   != nullptr   any        transfer to the awaiting coroutine; the frame
    //                            survives and the awaiter owns the Result
    //   == nullptr   true       nobody can ever read the Result; destroy now
    //   == nullptr   false      the caller still holds a Task<T>; park forever
    //                            and let ~Task destroy the frame
    //
    struct FinalAwaitable
    {
        // Always false: final_suspend must not report "ready", or the frame
        // would be destroyed by the runtime before the logic below runs.
        bool await_ready() noexcept { return false; }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
        {
            auto cont = h.promise().continuation;

            // "no continuation" is an explicit nullptr sentinel. Comparing handles
            // against std::noop_coroutine() to mean "unset" relied on an equality the
            // standard does not promise. Returning noop_coroutine() from here is a
            // different thing entirely -- it means "suspend forever" -- and stays.
            //
            // INVARIANT: continuation is written exactly once, by
            // TaskAwaiter::await_suspend, and only before the body ever runs
            // (initial_suspend is suspend_always). There is no race.
            if (cont == nullptr)
            {
                // Only a frame whose ownership was transferred to the scheduler (via
                // Task::release()) may self-destroy here. A Task the caller still
                // holds has no continuation too, and destroying it freed the frame
                // while Task::handle_ still pointed at it -- ASan reported
                // heap-use-after-free at task.hpp:62 (done()).
                //
                // INVARIANT: detached == true implies the Task handle that produced
                // this frame was emptied by release(). Nothing else can observe it.
                if (h.promise().detached)
                {
                    // Nobody will ever read this task's Result, so surface an error
                    // outcome here instead of silently dropping it at destroy time.
                    h.promise().log_discarded_result();
                    h.destroy();
                }
                // Not detached: park. ~Task() (or an in-flight await_resume) is
                // responsible for the destroy().
                return std::noop_coroutine();
            }

            // Otherwise, symmetrically transfer control back to the caller.
            //
            // INVARIANT: transferring here is safe with respect to `h` -- this
            // coroutine never runs again, so no one can touch the frame while the
            // awaiter is reading the Result out of it.
            return cont;
        }

        void await_resume() noexcept
        {
        }
    };

    // ============================================================================
    // TaskPromiseBase -- state shared by every Task<T>
    // ============================================================================
    //
    // Layout note: these members are the coroutine frame's payload, so every
    // one of them costs a few bytes in EVERY Task on the heap. Keep it small.
    // `next` is an atomic pointer because a task may be enqueued by one thread
    // and dequeued by another; nothing else here is touched cross-thread.
    //
    /// A task SHOULD not throw an exception
    struct TaskPromiseBase
    {
        // Set by TaskAwaiter::await_suspend before the body starts.
        // nullptr == "no coroutine is waiting for this one".
        std::coroutine_handle<> continuation{nullptr};

        // Set by Task::release(): frame ownership moved from a Task<T> to the
        // scheduler. THIS FLAG IS THE OWNERSHIP TOKEN -- see the header comment.
        bool detached{false};

        // Guards double-consumption: Task::get() and TaskAwaiter::await_resume()
        // move the result out, so a second read would hand back a moved-from value.
        bool result_taken{false};

        // intrusive MPSC queue hook (see detail/queue.hpp). Atomic: the producer
        // may be any thread, the consumer is always the owning worker.
        std::atomic<TaskPromiseBase*> next{nullptr};

        // The type-erased handle the event loop resumes. Parked here by
        // Task::release()/IO::schedule() because queue_ only carries the
        // TaskPromiseBase*, and the worker needs a full coroutine handle.
        std::coroutine_handle<> self_handle{nullptr};

        // Nothing runs until someone resumes us. This is what lets
        //   auto t = some_coroutine();   // build the frame, run nothing
        //   worker.schedule(std::move(t));
        // hand an unstarted coroutine to another thread.
        std::suspend_always initial_suspend() noexcept { return {}; }

        FinalAwaitable final_suspend() noexcept { return {}; }

        // A throwing Task is a contract violation: errors travel through Result<T>.
        // std::terminate rather than propagate, because there is no caller left
        // to catch -- the coroutine is detached by construction at that point.
        [[noreturn]] void unhandled_exception() noexcept
        {
            KIO_LOG_FATAL("unhandled exception in URing::Task (use Result<T> for errors)");
            std::terminate();
        }
    };

    // ============================================================================
    // TaskPromise<T> -- specialization for tasks that produce a value
    // ============================================================================
    //
    // Three return_value overloads exist so callers can write whichever reads
    // best; they all funnel into the same optional<Result<T>>.
    //
    //   result: nullopt  <-- body returned without co_return (UB per the
    //   |                    standard, but TaskAwaiter::await_resume and
    //   |                    Task::get() both treat it as operation_canceled
    //   |                    rather than dereferencing it)
    //   v
    //   engaged + has_value()   -- success, holds T
    //   engaged + !has_value()  -- error, holds Error
    //
    template <typename T>
    struct TaskPromise : TaskPromiseBase
    {
        std::optional<Result<T>> result;

        // Handle 'co_return value;'
        void return_value(T val) noexcept { result.emplace(std::move(val)); }

        // Handle 'co_return std::unexpected(Error::fail_errno(...));'
        // The error type of Result<T> is Error, not std::error_code.
        void return_value(std::unexpected<Error> err) noexcept { result.emplace(std::move(err)); }

        // Handle 'co_return Result<T>(...);'
        void return_value(Result<T> res) noexcept { result.emplace(std::move(res)); }

        // Called from FinalAwaitable just before a detached frame self-destructs.
        //
        // Only errors are logged: a detached task that succeeded had a result
        // nobody read, but that is the normal fire-and-forget case and is not
        // worth a log line. An error result that is dropped is a real bug in
        // the caller, which is why it is surfaced rather than discarded.
        void log_discarded_result() const noexcept
        {
            if (result.has_value() && !result->has_value()) [[unlikely]]
            {
                KIO_LOG_ERROR("detached Task discarded an error result: {} [{}]",
                              result->error().message(), result->error().context());
            }
        }

        Task<T> get_return_object() noexcept;
    };

    // ============================================================================
    // TaskPromise<void> -- specialization for side-effect tasks
    // ============================================================================
    //
    // Identical lifetime rules to TaskPromise<T>. There is no 'co_return value'
    // overload, so `co_return;` uses the implicit return_void() and leaves
    // `result` nullopt -- which the awaiter reports as operation_canceled.
    // That is why a void task that simply returns is indistinguishable from a
    // cancelled one at the await site.
    //
    template <>
    struct TaskPromise<void> : TaskPromiseBase
    {
        std::optional<Result<void>> result;

        // Handle 'co_return std::unexpected(Error::fail_errno(...));'
        void return_value(std::unexpected<Error> err) noexcept { result.emplace(std::move(err)); }

        // Handle 'co_return Result<void>(...);' or 'co_return {};'
        void return_value(Result<void> res) noexcept { result.emplace(std::move(res)); }

        // See TaskPromise<T>::log_discarded_result -- same rule, void flavour.
        void log_discarded_result() const noexcept
        {
            if (result.has_value() && !result->has_value()) [[unlikely]]
            {
                KIO_LOG_ERROR("detached Task<void> discarded an error result: {} [{}]",
                              result->error().message(), result->error().context());
            }
        }

        Task<void> get_return_object() noexcept;
    };
} // namespace kio::detail
