#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace aio
{
// -----------------------------------------------------------------------------
// task<T> - Minimal Coroutine Return Type
// -----------------------------------------------------------------------------

template <typename T = void>
class [[nodiscard("You must co_await a Task or keep it alive")]] Task
{
public:
    struct promise_type
    {
        std::optional<T> value;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                if (h.promise().continuation != nullptr)
                {
                    return h.promise().continuation;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~Task() noexcept
    {
        if (handle_ != nullptr)
        {
            handle_.destroy();
        }
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool Done() const { return handle_ && handle_.done(); }

    T Result()
    {
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(*handle_.promise().value);
    }

    void resume()
    {
        if (handle_ != nullptr && !handle_.done())
        {
            handle_.resume();
        }
    }

    // Alias resume for clarity: Starts the task concurrently.
    // WARNING: You must still keep the 'task' object alive!
    void Start() { resume(); }

    // Awaitable interface
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) noexcept
    {
        handle_.promise().continuation = cont;
        return handle_;
    }

    T await_resume()
    {
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(*handle_.promise().value);
    }

private:
    handle_type handle_;
};

template <>
class [[nodiscard("You must co_await a Task or keep it alive")]] Task<void>
{
public:
    struct promise_type
    {
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                if (h.promise().continuation)
                {
                    return h.promise().continuation;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~Task()
    {
        if (handle_ != nullptr)
        {
            handle_.destroy();
        }
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool Done() const { return handle_ && handle_.done(); }

    void Result()
    {
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
    }

    void resume()
    {
        if (handle_ && !handle_.done())
        {
            handle_.resume();
        }
    }

    // Alias resume for clarity: Starts the task concurrently.
    // WARNING: You must still keep the 'task' object alive!
    void Start() { resume(); }

    // Awaitable interface
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) noexcept
    {
        handle_.promise().continuation = cont;
        return handle_;
    }

    void await_resume()
    {
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
    }

private:
    handle_type handle_;
};
}  // namespace aio