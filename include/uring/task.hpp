#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "error.hpp"
#include "logger.hpp"

namespace URing
{
class IO;

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
            // We must destroy the frame here to prevent memory leaks.
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

    FinalAwaitable final_suspend() noexcept { return {}; }
};

template <typename T>
struct task_promise : task_promise_base
{
    std::optional<Result<T>> result;

    // Handle 'co_return value;'
    void return_value(T val) noexcept { result.emplace(std::move(val)); }

    // Handle 'co_return std::unexpected(err);'
    void return_value(Result<T> res) noexcept { result.emplace(std::move(res)); }

    // Handle 'co_return Result<T>(...);'
    void return_value(std::unexpected<std::error_code> err) noexcept { result.emplace(std::move(err)); }

    Task<T> get_return_object() noexcept;
};

template <>
struct task_promise<void> : task_promise_base
{
    std::optional<Result<void>> result_;

    // Handle 'co_return value;'
    void return_value(Result<void> res) noexcept
    {
        if (!res)
        {
            ALOG_ERROR("Detached task failed with error: {}", res.error().message());
        }
        result_.emplace(std::move(res));
    }

    // Handle 'co_return std::unexpected(err);'
    void return_value(std::unexpected<std::error_code> err) noexcept
    {
        ALOG_ERROR("Detached task failed with error: {}", err.error().message());
        result_.emplace(std::move(err));
    }

    // Handle 'co_return Result<T>(...);'
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
        // Only destroy if ownership was never transferred to the scheduler
        // or if the task has already completed.
        if (handle_)
        {
            handle_.destroy();
        }
    }

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

}  // namespace URing
