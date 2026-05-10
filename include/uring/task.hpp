#pragma once

#include <coroutine>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "error.hpp"

namespace URing
{

// Forward declarations
template <typename T>
struct Task;

class IoContext;

template <typename T>
struct task_promise
{
    // 1. Use std::optional so T isn't forced to have a default constructor.
    std::optional<Result<T>> result_;

    uint32_t pending_op_idx_ = UINT32_MAX;
    IoContext* ctx_ = nullptr;  // Set by the low-level awaitable

    // 2. Coroutine chain management
    std::coroutine_handle<> continuation_ = nullptr;
    bool detached_ = false;

    Task<T> get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept { return {}; }

    // 3. The Final Awaiter: This safely handles both Coroutine Chaining
    // and Deferred Destruction.
    struct final_awaiter
    {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<task_promise> h) noexcept
        {
            auto& p = h.promise();

            // If the Task was destroyed early, the memory frame is orphaned.
            // We safely destroy it here, AFTER the final kernel CQE has arrived.
            if (p.detached_)
            {
                h.destroy();
                return std::noop_coroutine();
            }

            // If another coroutine is co_awaiting this one, resume it now.
            if (p.continuation_)
            {
                return p.continuation_;
            }

            return std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

    final_awaiter final_suspend() noexcept { return {}; }

    // 4. C++20 constraint checks to fix the return_value/return_void compiler error.
    void return_value(T val) noexcept
        requires(!std::is_void_v<T>)
    {
        result_ = std::move(val);
    }

    void return_void() noexcept
        requires std::is_void_v<T>
    {
        result_ = Result<T>{};
    }

    void unhandled_exception() noexcept { result_ = std::unexpected(MakeErrorCode(EIO)); }
};

template <typename T>
struct Task
{
    using promise_type = task_promise<T>;
    std::coroutine_handle<promise_type> handle_;

    Task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}
    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}

    Task& operator=(Task&& o) noexcept
    {
        if (this != &o)
        {
            if (handle_)
                safe_destroy();
            handle_ = std::exchange(o.handle_, nullptr);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() noexcept
    {
        if (handle_)
            safe_destroy();
    }

    // Non-blocking check for manual polling systems.
    Result<T> get() const noexcept
    {
        if (!handle_ || !handle_.done() || !handle_.promise().result_.has_value())
            return std::unexpected(MakeErrorCode(EAGAIN));

        if constexpr (std::is_void_v<T>)
            return {};
        else
            return handle_.promise().result_.value();
    }

    bool done() const noexcept { return handle_ && handle_.done(); }

    // 5. Make the Task itself Awaitable so coroutines can co_await other Tasks.
    auto operator co_await() const noexcept
    {
        struct Awaiter
        {
            std::coroutine_handle<promise_type> handle_;

            bool await_ready() const noexcept { return handle_.done(); }

            // Suspend the caller, store it as the continuation, and run this task.
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
            {
                handle_.promise().continuation_ = caller;
                return handle_;
            }

            // Extract and return the final T value (or void)
            T await_resume()
            {
                auto& res = handle_.promise().result_;

                if (!res.has_value() || !res->has_value())  // outer optional, inner expected
                    throw std::runtime_error("Task failed or was cancelled");

                if constexpr (!std::is_void_v<T>)
                    return std::move(res->value());
            }
        };

        return Awaiter{handle_};
    }

private:
    // 6. Safe Destruction Logic
    void safe_destroy() noexcept
    {
        if (!handle_.done())
        {
            auto& p = handle_.promise();
            if (p.pending_op_idx_ != UINT32_MAX && p.ctx_)
            {
                // Async I/O is active. Issue the cancel command to the kernel.
                p.ctx_->request_cancel(p.pending_op_idx_);

                // Detach the frame. DO NOT call handle_.destroy() here.
                // The final_awaiter will destroy it when the kernel is actually done.
                p.detached_ = true;
            }
            else
            {
                // No pending async I/O. Safe to destroy immediately.
                handle_.destroy();
            }
        }
        else
        {
            // Task completed normally.
            handle_.destroy();
        }
    }
};

template <typename T>
Task<T> task_promise<T>::get_return_object() noexcept
{
    return Task<T>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

struct DetachedTask
{
    struct promise_type
    {
        DetachedTask get_return_object() noexcept { return {}; }

        // Don't suspend initially. Start executing the client handler immediately
        // until it hits its first co_await (e.g., waiting for data).
        std::suspend_never initial_suspend() noexcept { return {}; }

        // CRITICAL: std::suspend_never tells the C++ compiler to automatically
        // destroy the coroutine frame the moment the function reaches the end.
        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() noexcept
        {
            // Background tasks shouldn't bring down the server, but
            // you should log this in a real application.
            std::terminate();
        }
    };
};

}  // namespace URing