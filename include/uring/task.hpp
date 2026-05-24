#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "error.hpp"
#include "logger.hpp"

namespace URing
{

// Forward declarations
template <typename T>
struct Task;

// ============================================================================
// task_promise_base — Shared logic for all URing tasks
// ============================================================================
struct task_promise_base
{
    std::coroutine_handle<> continuation{std::noop_coroutine()};
    std::exception_ptr exception = nullptr;
    bool detached = false;

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
            // If the task was detached (released), it must self-destruct
            if (h.promise().detached)
            {
                h.destroy();
                return std::noop_coroutine();
            }

            // Otherwise, symmetrically transfer control back to the caller.
            return h.promise().continuation;
        }

        void await_resume() noexcept {}
    };

    FinalAwaitable final_suspend() noexcept { return {}; }
};

// ============================================================================
// task_promise<T> — Specialized for valued tasks
// ============================================================================
template <typename T>
struct task_promise : task_promise_base
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
struct task_promise<void> : task_promise_base
{
    std::optional<Result<void>> result_;

    // Handle 'co_return std::unexpected(err);'
    void return_value(std::unexpected<std::error_code> err) noexcept
    {
        // Log dropped errors for detached tasks
        if (detached)
        {
            ALOG_ERROR("Detached task failed: {}", err.error().message());
        }
        result_.emplace(std::move(err));
    }

    // Handle 'co_return Result<void>(...);' or 'co_return {};'
    void return_value(Result<void> res) noexcept
    {
        if (!res && detached)
        {
            ALOG_ERROR("Detached task failed: {}", res.error().message());
        }
        result_.emplace(std::move(res));
    }

    Task<void> get_return_object() noexcept;
};

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
                handle_.destroy();
            handle_ = std::exchange(o.handle_, {});
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task()
    {
        if (handle_)
            handle_.destroy();
    }

    // Transfers ownership of the coroutine frame to the scheduler.
    // The Task object becomes empty and the frame will self-destruct on completion.
    Handle release()
    {
        if (handle_)
        {
            handle_.promise().detached = true;
        }
        return std::exchange(handle_, {});
    }

    bool done() const noexcept { return handle_ && handle_.done(); }

    // -----------------------------------------------------------------------
    // co_await support — Returns Result<T> to the caller.
    // -----------------------------------------------------------------------
    auto operator co_await() noexcept
    {
        struct Awaiter
        {
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
                    std::rethrow_exception(p.exception);

                if (!p.result_.has_value())
                    return std::unexpected(MakeErrorCode(ECANCELED));

                return std::move(*p.result_);
            }
        };

        return Awaiter{handle_};
    }
};

template <typename T>
inline Task<T> task_promise<T>::get_return_object() noexcept
{
    return Task<T>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

inline Task<void> task_promise<void>::get_return_object() noexcept
{
    return Task<void>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

} // namespace URing
