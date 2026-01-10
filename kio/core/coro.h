#ifndef CORO_H
#define CORO_H

#include "async_logger.h"

#include <atomic>
#include <coroutine>
#include <exception>
#include <format>
#include <utility>
#include <variant>

namespace kio
{
template <typename T>
struct Task
{
    struct promise_type;

    std::coroutine_handle<promise_type> h_;

    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}

    Task(Task&& other) noexcept : h_(std::exchange(other.h_, {})) {}

    Task(const Task&) = delete;

    Task& operator=(const Task&) = delete;

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (h_)
                h_.destroy();
            h_ = other.h_;
            other.h_ = nullptr;
        }
        return *this;
    }

    ~Task()
    {
        if (h_)
        {
            h_.destroy();
        }
    }

    struct promise_type
    {
        std::variant<std::monostate, T, std::exception_ptr> result_;
        std::atomic<std::coroutine_handle<>> continuation_;  // ✅ ATOMIC

        [[nodiscard]]
        Task get_return_object() noexcept
        {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                // ✅ Load with acquire
                if (auto continuation = h.promise().continuation_.load(std::memory_order_acquire))
                    return continuation;
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void unhandled_exception() { result_.template emplace<2>(std::current_exception()); }

        void return_value(T value)
            requires(!std::is_void_v<T>)
        {
            result_.template emplace<1>(std::move(value));
        }
    };

    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return h_.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept
    {
        // ✅ Store with release
        h_.promise().continuation_.store(awaiting_coroutine, std::memory_order_release);
        return h_;
    }

    T await_resume()
    {
        if (h_.promise().result_.index() == 2)
        {
            std::rethrow_exception(std::get<2>(h_.promise().result_));
        }

        return std::get<1>(std::move(h_.promise().result_));
    }
};

// Specialization for Task<void>
template <>
struct Task<void>
{
    struct promise_type;

    std::coroutine_handle<promise_type> h_;

    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}

    Task(Task&& other) noexcept : h_(std::exchange(other.h_, {})) {}

    Task(const Task&) = delete;

    Task& operator=(const Task&) = delete;

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (h_)
                h_.destroy();
            h_ = other.h_;
            other.h_ = nullptr;
        }
        return *this;
    }

    ~Task()
    {
        if (h_)
        {
            h_.destroy();
        }
    }

    struct promise_type
    {
        std::variant<std::monostate, std::exception_ptr> result_;
        std::atomic<std::coroutine_handle<>> continuation_;  // ✅ ATOMIC

        [[nodiscard]]
        Task get_return_object() noexcept
        {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                // ✅ Load with acquire
                if (auto continuation = h.promise().continuation_.load(std::memory_order_acquire))
                    return continuation;
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void unhandled_exception() { result_ = std::current_exception(); }

        void return_void() {}
    };

    [[nodiscard]]
    bool await_ready() const noexcept
    {
        return h_.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept
    {
        // ✅ Store with release
        h_.promise().continuation_.store(awaiting_coroutine, std::memory_order_release);
        return h_;
    }

    void await_resume()
    {
        if (h_.promise().result_.index() == 1)
        {
            std::rethrow_exception(std::get<1>(h_.promise().result_));
        }
    }
};

/**
 * @brief A task that starts immediately and destroys itself on completion.
 * Used for fire-and-forget operations like connection handlers.
 */
struct DetachedTask
{
    struct promise_type  // NOLINT
    {
        DetachedTask get_return_object() noexcept { return {}; }  // NOLINT

        // Start immediately - this is fire-and-forget
        std::suspend_never initial_suspend() noexcept { return {}; }  // NOLINT

        // Self-destruct on completion
        // The coroutine frame is destroyed automatically after this returns
        std::suspend_never final_suspend() noexcept { return {}; }  // NOLINT

        void return_void() noexcept {}  // NOLINT

        void unhandled_exception() noexcept  // NOLINT
        {
            try
            {
                throw;
            }
            catch (const std::exception& e)
            {
                ALOG_ERROR("DetachedTask exception: {}", e.what());
            }
            catch (...)
            {
                ALOG_ERROR("DetachedTask unknown exception");
            }
        }
    };

    // No handle storage needed - coroutine is truly detached
    // Constructor does nothing, destructor does nothing
    DetachedTask() = default;
};
}  // namespace kio

#endif  // CORO_H