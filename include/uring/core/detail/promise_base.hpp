#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <optional>

#include "uring/error.hpp"
#include "uring/logger.hpp"

namespace URing
{
class IO;
// Forward declarations
template <typename T>
struct Task;

}  // namespace URing

namespace URing::detail
{
// -----------------------------------------------------------------------
// FinalAwaitable — Return to the parent or notify the owning reactor.
// -----------------------------------------------------------------------
struct FinalAwaitable
{
    bool await_ready() noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
    {
        if (auto cont = h.promise().continuation)
        {
            return cont;
        }
        if (auto complete = h.promise().on_complete)
        {
            complete(h.promise());
        }
        return std::noop_coroutine();
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
    // intrusive queue hook
    std::atomic<TaskPromiseBase*> next{nullptr};
    // The type-erased handle used by the event loop to resume this frame
    std::coroutine_handle<> self_handle{nullptr};
    IO* owner{nullptr};
    // Singly-linked destroy-queue hook, used only once a root task completes
    // (see IO::completed_ / IO::reap_completed).
    TaskPromiseBase* root_next{nullptr};
    void (*on_complete)(TaskPromiseBase&) noexcept {nullptr};
    bool started{false};

    struct InitialAwaitable
    {
        TaskPromiseBase& promise;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept { promise.started = true; }
    };
    InitialAwaitable initial_suspend() noexcept { return {*this}; }
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

    // Handle 'co_return std::unexpected(err);'
    void return_value(std::unexpected<Error> err) noexcept { result.emplace(std::move(err)); }

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
    void return_value(std::unexpected<Error> err) noexcept { result.emplace(std::move(err)); }

    // Handle 'co_return Result<void>(...);' or 'co_return {};'
    void return_value(Result<void> res) noexcept { result.emplace(std::move(res)); }

    Task<void> get_return_object() noexcept;
};
}  // namespace URing::detail
