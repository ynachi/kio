#pragma once

#include <coroutine>
#include <exception>

#include "uring/error.hpp"
#include "uring/logger.hpp"

namespace URing
{
// Forward declarations
template <typename T>
struct Task;

}  // namespace URing

namespace URing::detail
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

        // If continuation is noop, it means this task was "detached" (scheduled).
        if (cont == std::noop_coroutine())
        {
            h.destroy();
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
    std::coroutine_handle<> continuation{std::noop_coroutine()};
    // intrusive queue hook
    std::atomic<TaskPromiseBase*> next{nullptr};
    // The type-erased handle used by the event loop to resume this frame
    std::coroutine_handle<> self_handle{nullptr};

    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaitable final_suspend() noexcept { return {}; }

    // A throwing Task is a contract violation: errors travel through Result<T>.
    [[noreturn]] void unhandled_exception() noexcept
    {
        ALOG_FATAL("unhandled exception in URing::Task (use Result<T> for errors)");
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

    // Handle 'co_return std::unexpected(err);'
    void return_value(std::unexpected<std::error_code> err) noexcept { result.emplace(std::move(err)); }

    // Handle 'co_return Result<T>(...);'
    void return_value(Result<T> res) noexcept { result.emplace(std::move(res)); }

    Task<T> get_return_object() noexcept;
};

// ============================================================================
// task_promise<void> — Specialized for side-effect tasks
// ============================================================================
template <>
struct TaskPromise<void> : TaskPromiseBase
{
    std::optional<Result<void>> result;

    // Handle 'co_return std::unexpected(err);'
    void return_value(std::unexpected<std::error_code> err) noexcept { result.emplace(std::move(err)); }

    // Handle 'co_return Result<void>(...);' or 'co_return {};'
    void return_value(Result<void> res) noexcept { result.emplace(std::move(res)); }

    Task<void> get_return_object() noexcept;
};
}  // namespace URing::detail