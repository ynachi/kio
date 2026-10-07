#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <optional>

#include "uring/error.hpp"
#include "uring/logger.hpp"

namespace kio
{
// Forward declarations
template <typename T>
struct Task;

}  // namespace kio

namespace kio::detail
{
// -----------------------------------------------------------------------
// FinalAwaitable — The "Self-Cleaning" Mechanism
// -----------------------------------------------------------------------
struct FinalAwaitable
{
    bool await_ready() noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
    {
        auto cont = h.promise().continuation;

        // "no continuation" is an explicit nullptr sentinel. Comparing handles
        // against std::noop_coroutine() to mean "unset" relied on an equality the
        // standard does not promise. Returning noop_coroutine() from here is a
        // different thing entirely -- it means "suspend forever" -- and stays.
        if (cont == nullptr)
        {
            // Only a frame whose ownership was transferred to the scheduler (via
            // Task::release()) may self-destroy here. A Task the caller still
            // holds has no continuation too, and destroying it freed the frame
            // while Task::handle_ still pointed at it -- ASan reported
            // heap-use-after-free at task.hpp:62 (done()).
            if (h.promise().detached)
            {
                // Nobody will ever read this task's Result, so surface an error
                // outcome here instead of silently dropping it at destroy time.
                h.promise().log_discarded_result();
                h.destroy();
            }
            return std::noop_coroutine();
        }

        // Otherwise, symmetrically transfer control back to the caller.
        return cont;
    }

    void await_resume() noexcept {}
};

// ============================================================================
// task_promise_base — Shared logic for all URing tasks
// ============================================================================
/// A task SHOULD not throw an exception
struct TaskPromiseBase
{
    std::coroutine_handle<> continuation{nullptr};
    // Set by Task::release(): frame ownership moved to the scheduler.
    bool detached{false};
    // Guards double-consumption: Task::get() and TaskAwaiter::await_resume()
    // move the result out, so a second read would hand back a moved-from value.
    bool result_taken{false};
    // intrusive queue hook
    std::atomic<TaskPromiseBase*> next{nullptr};
    // The type-erased handle used by the event loop to resume this frame
    std::coroutine_handle<> self_handle{nullptr};

    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaitable final_suspend() noexcept { return {}; }

    // A throwing Task is a contract violation: errors travel through Result<T>.
    [[noreturn]] void unhandled_exception() noexcept
    {
        KIO_LOG_FATAL("unhandled exception in URing::Task (use Result<T> for errors)");
        std::terminate();
    }
};

// ============================================================================
// task_promise<T> — Specialized for valued tasks
// ============================================================================
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
// task_promise<void> — Specialized for side-effect tasks
// ============================================================================
template <>
struct TaskPromise<void> : TaskPromiseBase
{
    std::optional<Result<void>> result;

    // Handle 'co_return std::unexpected(Error::fail_errno(...));'
    void return_value(std::unexpected<Error> err) noexcept { result.emplace(std::move(err)); }

    // Handle 'co_return Result<void>(...);' or 'co_return {};'
    void return_value(Result<void> res) noexcept { result.emplace(std::move(res)); }

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
}  // namespace kio::detail