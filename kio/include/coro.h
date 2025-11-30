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
        struct promise_type;

        std::coroutine_handle<promise_type> h_;

        explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}

        Task(Task &&other) noexcept : h_(std::exchange(other.h_, {})) {}

        Task(const Task &) = delete;

        Task &operator=(const Task &) = delete;

        Task &operator=(Task &&other) noexcept
        {
            if (this != &other)
            {
                if (h_) h_.destroy();
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
            std::coroutine_handle<> continuation_;

            [[nodiscard]]
            Task get_return_object() noexcept
            {
                return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // we are creating lazy coroutines
            // Should be std::suspend_always to make the task "lazy" (it doesn't run until awaited).
            std::suspend_always initial_suspend() noexcept { return {}; }
            // Crucially, this awaiter must resume any other coroutine that is co_awaiting this task's completion. This
            // is how you chain asynchronous operations together.
            // This awaiter is crucial for chaining coroutines.
            // When this task completes, it resumes whoever was awaiting it.
            struct final_awaiter
            {
                bool await_ready() noexcept { return false; }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
                {
                    // Resume the continuation or return to the original caller if there is none.
                    if (auto continuation = h.promise().continuation_) return continuation;
                    return std::noop_coroutine();
                }

                void await_resume() noexcept {}
            };

            final_awaiter final_suspend() noexcept { return {}; }
            // Stores the exception pointer so it can be re-thrown by the awaiting coroutine.
            void unhandled_exception() { result_ = std::current_exception(); }

            // Stores the final result in the promise so the awaiting coroutine can retrieve it
            // Conditionally provide return_value OR return_void, not both
            void return_value(T value)
                requires(!std::is_void_v<T>)
            {
                result_ = std::move(value);
            }
        };

        [[nodiscard]]
        bool await_ready() const noexcept
        {
            // A task is ready if it's already done.
            return h_.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept
        {
            // Store the awaiting coroutine as our continuation
            h_.promise().continuation_ = awaiting_coroutine;
            // Resume our handle to start the task's execution
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
    template<>
    struct Task<void>
    {
        struct promise_type;

        std::coroutine_handle<promise_type> h_;

        explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}

        Task(Task &&other) noexcept : h_(std::exchange(other.h_, {})) {}

        Task(const Task &) = delete;

        Task &operator=(const Task &) = delete;

        Task &operator=(Task &&other) noexcept
        {
            if (this != &other)
            {
                if (h_) h_.destroy();
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
            std::coroutine_handle<> continuation_;

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
                    if (auto continuation = h.promise().continuation_) return continuation;
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
            h_.promise().continuation_ = awaiting_coroutine;
            return h_;
        }

        // await_resume returns void
        void await_resume()
        {
            if (h_.promise().result_.index() == 1)
            {
                std::rethrow_exception(std::get<1>(h_.promise().result_));
            }
        }
    };


    struct DetachedTask
    {
        struct promise_type
        {
            DetachedTask get_return_object() noexcept { return DetachedTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }

            std::suspend_never initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }

            void return_void() noexcept {}

            void unhandled_exception() noexcept
            {
                // TODO: better manage this exception
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

        using handle_t = std::coroutine_handle<promise_type>;
        handle_t coro;

        explicit DetachedTask(handle_t h) : coro(h) {}

        DetachedTask(DetachedTask &&o) noexcept : coro(std::exchange(o.coro, {})) {}

        ~DetachedTask()
        {
            if (coro) coro.destroy();
        }

        void detach() && noexcept
        {
            // Forget the handle → let coroutine run to completion
            coro = {};
        }
    };
}  // namespace kio

#endif  // CORO_H
