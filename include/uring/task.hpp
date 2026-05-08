#pragma once

#include <cassert>
#include <coroutine>
#include <optional>
#include <type_traits>
#include <utility>

#include "token.hpp"

namespace URing
{
template <typename T>
struct PromiseVariant
{
    static_assert(!std::is_reference_v<T>, "Task<T> does not support reference result types");

    std::optional<T> value;

    void return_value(T v) noexcept(std::is_nothrow_move_constructible_v<T>) { value.emplace(std::move(v)); }
};

template <>
struct PromiseVariant<void>
{
    void return_void() noexcept {}
};

template <typename T = void>
class [[nodiscard("You must co_await a Task or keep it alive")]] Task
{
public:
    struct promise_type : PromiseVariant<T>
    {
        std::coroutine_handle<> continuation = nullptr;

        Task get_return_object() noexcept { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter
        {
            bool await_ready() const noexcept { return false; }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                if (auto cont = h.promise().continuation)
                {
                    return cont;
                }
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }

        void unhandled_exception() noexcept { std::terminate(); }

        void* operator new(std::size_t size) { return tl_coro_pool.allocate(size); }
        void operator delete(void* ptr, std::size_t size) { tl_coro_pool.deallocate(ptr, size); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type handle) noexcept : handle_(handle) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() noexcept { destroy(); }

    bool done() const noexcept
    {
        assert(handle_ != nullptr && "Attempted to query a moved-from Task");
        return handle_.done();
    }

    void resume()
    {
        assert(handle_ != nullptr && "Attempted to resume a moved-from Task");
        if (!handle_.done())
        {
            handle_.resume();
        }
    }

    bool await_ready() const noexcept { return done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
    {
        assert(handle_ != nullptr && "Attempted to co_await a moved-from Task");
        handle_.promise().continuation = caller;
        return handle_;
    }

    auto await_resume()
    {
        assert(handle_ != nullptr && "Attempted to resume a moved-from Task");
        assert(handle_.done() && "Attempted to resume a Task before completion");

        if constexpr (!std::is_void_v<T>)
        {
            assert(handle_.promise().value.has_value() && "Task finished without returning a value!");
            return std::move(*handle_.promise().value);
        }
    }

    auto result()
    {
        assert(handle_ != nullptr && "Attempted to read the result of a moved-from Task");
        assert(handle_.done() && "Attempted to read the result of a Task that has not completed");
        if constexpr (!std::is_void_v<T>)
        {
            return std::move(*handle_.promise().value);
        }
    }

private:
    void destroy() noexcept
    {
        if (handle_ != nullptr)
        {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    handle_type handle_ = nullptr;
};

}  // namespace URing
