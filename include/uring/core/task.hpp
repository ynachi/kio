#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "detail/promise_base.hpp"
#include "uring/error.hpp"
#include "uring/logger.hpp"

namespace URing
{

// Forward declarations
template <typename T>
struct Task;

// ============================================================================
// task_promise<T> — Specialized for valued tasks
// ============================================================================
template <typename T>
struct task_promise : detail::TaskPromiseBase
{
    std::optional<Result<T>> result_;

    // Handle 'co_return value;'
    void return_value(T val) noexcept { result_.emplace(std::move(val)); }

    // Handle 'co_return std::unexpected(err);'
    void return_value(std::unexpected<std::error_code> err) noexcept { result_.emplace(std::move(err)); }

    // Handle 'co_return Result<T>(...);'
    void return_value(Result<T> res) noexcept { result_.emplace(std::move(res)); }

    Task<T> get_return_object() noexcept;
};

// ============================================================================
// task_promise<void> — Specialized for side-effect tasks
// ============================================================================
template <>
struct task_promise<void> : detail::TaskPromiseBase
{
    std::optional<Result<void>> result_;

    // Handle 'co_return std::unexpected(err);'
    void return_value(std::unexpected<std::error_code> err) noexcept { result_.emplace(std::move(err)); }

    // Handle 'co_return Result<void>(...);' or 'co_return {};'
    void return_value(Result<void> res) noexcept { result_.emplace(std::move(res)); }

    Task<void> get_return_object() noexcept;
};

namespace detail
{
template <typename T>
struct TaskAwaiter
{
    using Handle = std::coroutine_handle<task_promise<T>>;
    Handle handle_;

    bool await_ready() const noexcept { return handle_.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
    {
        handle_.promise().continuation = caller;
        return handle_;
    }

    Result<T> await_resume()
    {
        auto& p = handle_.promise();

        if (p.exception)
        {
            std::rethrow_exception(p.exception);
        }

        if (!p.result_.has_value())
        {
            return std::unexpected(MakeErrorCode(ECANCELED));
        }

        return std::move(*p.result_);
    }
};
}  // namespace detail

// ============================================================================
// Task<T> — The Primary Coroutine Type
// ============================================================================
template <typename T>
struct [[nodiscard]] Task
{
    using promise_type = task_promise<T>;
    using Handle = std::coroutine_handle<promise_type>;
    Handle handle_;

    explicit Task(Handle h) noexcept : handle_(h) {}
    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}

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

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task()
    {
        if (handle_)
        {
            handle_.destroy();
        }
    }

    // Transfers ownership of the coroutine frame to the scheduler.
    // The Task object becomes empty and the frame will self-destruct on completion.
    Handle release() { return std::exchange(handle_, {}); }

    bool done() const noexcept { return handle_ && handle_.done(); }

    // -----------------------------------------------------------------------
    // co_await support — Returns Result<T> to the caller.
    // -----------------------------------------------------------------------
    auto operator co_await() noexcept { return detail::TaskAwaiter{handle_}; }
};

template <typename T>
Task<T> task_promise<T>::get_return_object() noexcept
{
    return Task<T>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

inline Task<void> task_promise<void>::get_return_object() noexcept
{
    return Task{std::coroutine_handle<task_promise>::from_promise(*this)};
}

}  // namespace URing
