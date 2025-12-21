#ifndef CORO_H
#define CORO_H

#include <coroutine>
#include <exception>
#include <format>
#include <utility>
#include <variant>

#include "async_logger.h"

namespace kio
{

template<typename T>
struct Task
{
    struct promise_type;  // NOLINT

    std::coroutine_handle<promise_type> h;

    explicit Task(std::coroutine_handle<promise_type> h) : h(h) {}

    Task(Task &&other) noexcept : h(std::exchange(other.h, {})) {}

    Task(const Task &) = delete;

    Task &operator=(const Task &) = delete;

    Task &operator=(Task &&other) noexcept
    {
        if (this != &other)
        {
            if (h)
            {
                h.destroy();
            }
            h = other.h;
            other.h = nullptr;
        }
        return *this;
    }

    ~Task() noexcept
    {
        if (h && !h.done())
        {
            h.destroy();
        }
    }

    struct promise_type
    {
        std::variant<std::monostate, T, std::exception_ptr> result;
        std::coroutine_handle<> continuation;

        [[nodiscard]]
        Task get_return_object() noexcept  // NOLINT
        {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // we are creating lazy coroutines
        // Should be std::suspend_always to make the task "lazy" (it doesn't run until awaited).
        std::suspend_always initial_suspend() noexcept { return {}; }  // NOLINT
        // Crucially, this awaiter must resume any other coroutine that is co_awaiting this task's completion. This
        // is how you chain asynchronous operations together.
        // This awaiter is crucial for chaining coroutines.
        // When this task completes, it resumes whoever was awaiting it.
        struct FinalAwaiter
        {
            bool await_ready() noexcept { return false; }  // NOLINT

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept  // NOLINT
            {
                // Resume the continuation or return to the original caller if there is none.
                if (auto continuation = h.promise().continuation)
                {
                    return continuation;
                }
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}  // NOLINT
        };

        FinalAwaiter final_suspend() noexcept { return {}; }  // NOLINT
        // Stores the exception pointer so it can be re-thrown by the awaiting coroutine.
        void unhandled_exception() { result.template emplace<2>(std::current_exception()); }  // NOLINT

        // Stores the final result in the promise so the awaiting coroutine can retrieve it
        // Conditionally provide return_value OR return_void, not both
        void return_value(T value)  // NOLINT
            requires(!std::is_void_v<T>)
        {
            result.template emplace<1>(std::move(value));
        }
    };

    [[nodiscard]]
    bool await_ready() const noexcept  // NOLINT
    {
        // A task is ready if it's already done.
        return h.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept  // NOLINT
    {
        // Store the awaiting coroutine as our continuation
        h.promise().continuation = awaiting_coroutine;
        // Resume our handle to start the task's execution
        return h;
    }

    T await_resume()  // NOLINT
    {
        if (h.promise().result.index() == 2)
        {
            std::rethrow_exception(std::get<2>(h.promise().result));
        }

        return std::get<1>(std::move(h.promise().result));
    }
};

// Specialization for Task<void>
template<>
struct Task<void>
{
    struct promise_type;  // NOLINT

    std::coroutine_handle<promise_type> h;

    explicit Task(std::coroutine_handle<promise_type> h) : h(h) {}

    Task(Task &&other) noexcept : h(std::exchange(other.h, {})) {}

    Task(const Task &) = delete;

    Task &operator=(const Task &) = delete;

    Task &operator=(Task &&other) noexcept
    {
        if (this != &other)
        {
            if (h)
            {
                h.destroy();
            }
            h = other.h;
            other.h = nullptr;
        }
        return *this;
    }

    ~Task()
    {
        if (h && !h.done())
        {
            h.destroy();
        }
    }

    struct promise_type
    {
        std::variant<std::monostate, std::exception_ptr> result;
        std::coroutine_handle<> continuation;

        [[nodiscard]]
        Task get_return_object() noexcept  // NOLINT
        {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }  // NOLINT

        struct FinalAwaiter
        {
            bool await_ready() noexcept { return false; }  // NOLINT

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept  // NOLINT
            {
                if (auto continuation = h.promise().continuation)  // NOLINT
                {
                    return continuation;
                }
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}  // NOLINT
        };

        FinalAwaiter final_suspend() noexcept { return {}; }  // NOLINT
        void unhandled_exception() { result = std::current_exception(); }  // NOLINT

        void return_void() {}  // NOLINT
    };

    [[nodiscard]]
    bool await_ready() const noexcept  // NOLINT
    {
        return h.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept  // NOLINT
    {
        h.promise().continuation = awaiting_coroutine;
        return h;
    }

    // await_resume returns void
    void await_resume()  // NOLINT
    {
        if (h.promise().result.index() == 1)
        {
            std::rethrow_exception(std::get<1>(h.promise().result));
        }
    }
};

struct DetachedTask
{
    struct promise_type  // NOLINT
    {
        DetachedTask get_return_object() noexcept { return {}; }  // NOLINT

        // Start immediately - this is fire-and-forget
        std::suspend_never initial_suspend() noexcept { return {}; }  // NOLINT

        // Self-destruct on completion
        std::suspend_never final_suspend() noexcept { return {}; }  // NOLINT

        void return_void() noexcept {}  // NOLINT

        void unhandled_exception() noexcept  // NOLINT
        {
            try
            {
                throw;
            }
            catch (const std::exception &e)
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
