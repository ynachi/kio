#pragma once
#include <coroutine>
#include <optional>
#include <type_traits>
#include <utility>

#include "context.h"

namespace kio
{
    template <class T = void>
    class Task_v2;

    namespace detail
    {
        struct PromiseBase
        {
            std::coroutine_handle<> cont = std::noop_coroutine();

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct Final
            {
                bool await_ready() const noexcept { return false; }

                template <class P>
                std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept
                {
                    return h.promise().cont; // symmetric transfer to whoever awaited us
                }

                void await_resume() const noexcept
                {
                }
            };

            Final final_suspend() noexcept { return {}; }
            [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
        };

        template <class T>
        struct ResultMixin
        {
            std::optional<T> value;

            template <class U>
            void return_value(U&& v) noexcept(std::is_nothrow_constructible_v<T, U>)
            {
                value.emplace(std::forward<U>(v));
            }
        };

        template <>
        struct ResultMixin<void>
        {
            void return_void() noexcept
            {
            }
        };

        template <class T>
        struct Promise : PromiseBase, ResultMixin<T>
        {
            Task_v2<T> get_return_object() noexcept
            {
                return Task_v2<T>(std::coroutine_handle<Promise>::from_promise(*this));
            }
        };
    } // namespace detail

    template <class T>
    class [[nodiscard]] Task_v2
    {
    public:
        using promise_type = detail::Promise<T>;

        Task_v2() = default;

        explicit Task_v2(std::coroutine_handle<promise_type> h) noexcept : h_(h)
        {
        }

        Task_v2(Task_v2&& o) noexcept : h_(std::exchange(o.h_, {}))
        {
        }

        Task_v2& operator=(Task_v2&& o) noexcept
        {
            if (this != &o)
            {
                reset();
                h_ = std::exchange(o.h_, {});
            }
            return *this;
        }

        Task_v2(const Task_v2&) = delete;
        Task_v2& operator=(const Task_v2&) = delete;
        ~Task_v2() { reset(); }

        bool await_ready() const noexcept { return !h_ || h_.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
        {
            h_.promise().cont = caller;
            return h_;
        }

        T await_resume() noexcept
        {
            if constexpr (!std::is_void_v<T>) return std::move(*h_.promise().value);
        }

    private:
        void reset() noexcept
        {
            if (h_)
            {
                h_.destroy();
                h_ = {};
            }
        }

        std::coroutine_handle<promise_type> h_{};
    };

    namespace detail
    {
        struct Detached
        {
            struct promise_type
            {
                // Removed `Op start;` here. We can just queue the handle directly!
                Detached get_return_object() noexcept
                {
                    return Detached{std::coroutine_handle<promise_type>::from_promise(*this)};
                }

                std::suspend_always initial_suspend() noexcept { return {}; }
                std::suspend_never final_suspend() noexcept { return {}; }

                void return_void() noexcept
                {
                }

                [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
            };

            std::coroutine_handle<promise_type> h;
        };

        template <class T>
        Detached run_detached(Task_v2<T> t)
        {
            (void)co_await t;
        }
    } // namespace detail

    namespace io
    {
        template <class T>
        void Context::spawn(Task_v2<T> task) noexcept
        {
            auto detached = detail::run_detached<T>(std::move(task));
            post(detached.h);
        }
    }
} // namespace kio
