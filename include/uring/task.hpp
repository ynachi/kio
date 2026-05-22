#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "error.hpp"
#include "logger.hpp"

namespace URing
{
class IoWorker;

// Forward declarations
template <typename T>
struct Task;

struct task_promise_base
{
    std::coroutine_handle<> continuation{std::noop_coroutine()};
    std::exception_ptr exception = nullptr;

    std::suspend_always initial_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept { exception = std::current_exception(); }

    // -----------------------------------------------------------------------
    // final_suspend — symmetric transfer to continuation
    // Keeps stack depth O(1) via tail-call optimization.
    // -----------------------------------------------------------------------
    struct FinalAwaitable
    {
        bool await_ready() noexcept { return false; }
        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
        {
            return h.promise().continuation;
        }
        void await_resume() noexcept {}
    };

    FinalAwaitable final_suspend() noexcept { return {}; }
};

template <typename T>
struct task_promise : task_promise_base
{
    std::optional<Result<T>> result_;

    // Task<T> always stores and exposes Result<T>. Prefer Task<T>, not
    // Task<Result<T>>: returning Result<T> here is flattened into the task's
    // own result channel instead of nesting Result<Result<T>>.
    void return_value(T val) noexcept { result_.emplace(std::move(val)); }
    void return_value(Result<T> val) noexcept { result_.emplace(std::move(val)); }
    void return_value(std::unexpected<std::error_code> err) noexcept { result_.emplace(std::move(err)); }
    Task<T> get_return_object() noexcept;
};

template <>
struct task_promise<void> : task_promise_base
{
    std::optional<Result<void>> result_;

    // A Task<void> still completes with Result<void>. Use `co_return {};` or
    // `co_return Result<void>{};` for success, and `co_return std::unexpected(...)`
    // for failure. Plain `co_return;` is intentionally not supported because the
    // promise needs return_value() for the error channel.
    void return_value(Result<void> val) noexcept { result_.emplace(std::move(val)); }
    void return_value(std::unexpected<std::error_code> err) noexcept { result_.emplace(std::move(err)); }
    Task<void> get_return_object() noexcept;
};

// Lazy coroutine task.
//
// Contract:
//   Task<T>::get()      -> Result<T>
//   co_await Task<T>    -> Result<T>
//
// The task itself owns the error channel. Prefer Task<T> and return errors with
// `co_return std::unexpected(error);`. Avoid Task<Result<T>> unless you
// intentionally want a nested Result<Result<T>> payload.
template <typename T>
struct Task
{
    using Handle = std::coroutine_handle<task_promise<T>>;
    Handle handle_;

    explicit Task(Handle h) noexcept : handle_(h) {}
    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}

    Task& operator=(Task&& o) noexcept
    {
        if (this != &o)
        {
            if (handle_ && handle_.done())
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
        // Only destroy if ownership was never transferred to the scheduler
        // or if the task has already completed.
        if (handle_)
        {
            handle_.destroy();
        }
    }

    // Non-blocking check for manual polling systems. Returns EWOULDBLOCK until the
    // coroutine has completed and then returns the stored Result<T>.
    Result<T> get() const noexcept
    {
        if (!handle_ || !handle_.done() || !handle_.promise().result_.has_value())
        {
            return std::unexpected(MakeErrorCode(EWOULDBLOCK));
        }
        return handle_.promise().result_.value();
    }

    bool done() const noexcept { return handle_ && handle_.done(); }

    Handle release() { return std::exchange(handle_, {}); }

    // Awaiting a Task<T> returns Result<T>, matching get(). This keeps errors
    // explicit at every composition point:
    //
    //   auto value = co_await child();
    //   if (!value) co_return std::unexpected(value.error());
    //
    // Avoid Task<Result<T>>; Task<T> already carries Result<T>.
    auto operator co_await() noexcept
    {
        struct Awaiter
        {
            Handle handle_;

            bool await_ready() const noexcept { return handle_.done(); }

            // Suspend the caller, store it as the continuation, and run this task.
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
            {
                handle_.promise().continuation = caller;
                return handle_;
            }

            // Extract and return the final Result<T>.
            Result<T> await_resume()
            {
                auto& p = handle_.promise();

                // If the coroutine itself threw, rethrow — this is not an expected I/O error.
                if (p.exception_)
                {
                    std::rethrow_exception(p.exception_);
                }

                if (!p.result_.has_value())
                {
                    return std::unexpected{MakeErrorCode(ECANCELED)};
                }
                return std::move(*p.result_);
            }
        };

        return Awaiter{handle_};
    }
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

struct DetachedTask
{
    struct promise_type
    {
        DetachedTask get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept
        {
            ALOG_FATAL("detached task died with unhandled exception");
            ALOG::stop();
            std::terminate();
        }
    };
};

}  // namespace URing
