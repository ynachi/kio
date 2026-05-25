#pragma once

#include <coroutine>
#include <utility>

#include "detail/promise_base.hpp"
#include "detail/task_awaiter.hpp"

namespace URing
{
// ============================================================================
// Task<T> — The Primary Coroutine Type
// ============================================================================
template <typename T>
struct [[nodiscard]] Task
{
    using promise_type = detail::TaskPromise<T>;
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
Task<T> detail::TaskPromise<T>::get_return_object() noexcept
{
    return Task<T>{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

inline Task<void> detail::TaskPromise<void>::get_return_object() noexcept
{
    return Task{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

}  // namespace URing
