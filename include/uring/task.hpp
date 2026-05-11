#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "error.hpp"
#include "logger.hpp"

namespace URing
{

// Forward declarations
template <typename T>
struct Task;

class IoContext;

struct task_promise_base
{
    std::coroutine_handle<> continuation_ = nullptr;
    std::exception_ptr exception_ = nullptr;
    bool started_ = false;

    struct initial_awaiter
    {
        task_promise_base* base_;

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept { base_->started_ = true; }
    };

    initial_awaiter initial_suspend() noexcept { return initial_awaiter{this}; }
    void unhandled_exception() noexcept { exception_ = std::current_exception(); }

    struct final_awaiter
    {
        bool await_ready() const noexcept { return false; }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
        {
            auto& p = h.promise();
            if (p.continuation_)
                return p.continuation_;
            return std::noop_coroutine();
        }
        void await_resume() noexcept {}
    };

    final_awaiter final_suspend() noexcept { return {}; }

    void* operator new(std::size_t size) noexcept { return CoroAllocator::allocate(size); }  // NOLINT

    void operator delete(void* ptr, std::size_t size) noexcept { CoroAllocator::deallocate(ptr, size); }
};

template <typename T>
struct task_promise : task_promise_base
{
    std::optional<Result<T>> result_;
    void return_value(T val) noexcept { result_.emplace(std::move(val)); }
    Task<T> get_return_object() noexcept;
};

template <>
struct task_promise<void> : task_promise_base
{
    std::optional<Result<void>> result_;
    void return_void() noexcept { result_.emplace(Result<void>{}); }
    Task<void> get_return_object() noexcept;
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
        {
            return std::unexpected(MakeErrorCode(EAGAIN));
        }

        if constexpr (std::is_void_v<T>)
        {
            return {};
        }
        else
        {
            return handle_.promise().result_.value();
        }
    }

    bool done() const noexcept { return handle_ && handle_.done(); }

    // 5. Make the Task itself Awaitable so coroutines can co_await other Tasks.
    auto operator co_await() noexcept
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
                auto& p = handle_.promise();

                // If the code itself crashed (e.g., out of memory), we should
                // probably still throw because this isn't an "expected" I/O error.
                if (p.exception_)
                {
                    std::rethrow_exception(p.exception_);
                }

                if (!p.result_.has_value())
                {
                    return std::unexpected(MakeErrorCode(ECANCELED));
                }

                if constexpr (std::is_void_v<T>)
                    return {};
                else
                    return *p.result_;
            }
        };

        return Awaiter{handle_};
    }

private:
    // 6. Safe Destruction Logic
    void safe_destroy() noexcept
    {
        if (handle_ == nullptr)
        {
            return;
        }

        if (handle_.done())
        {
            handle_.destroy();
            return;
        }

        auto& p = handle_.promise();

        // Destroying a started coroutine before completion leaves reactor queues
        // or child-task continuations with dangling coroutine handles.
        if (p.started_)
        {
            ALOG_ERROR("destroying a started Task before completion is a bug");
            std::terminate();
        }

        handle_.destroy();
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

        // Don't suspend initially. Start executing the client handler immediately
        // until it hits its first co_await (e.g., waiting for data).
        std::suspend_never initial_suspend() noexcept { return {}; }

        // CRITICAL: std::suspend_never tells the C++ compiler to automatically
        // destroy the coroutine frame the moment the function reaches the end.
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
