//
// Created by Yao ACHI on 10/01/2026.
//

#ifndef KIO_CORE_TEST_HELPERS_H
#define KIO_CORE_TEST_HELPERS_H

#include "kio/core/coro.h"
#include "kio/core/worker.h"

#include <latch>
#include <variant>

namespace kio::testing
{

// A synchronization event that blocks the main thread
class SyncWaitEvent
{
    std::latch latch_{1};

public:
    void set() noexcept { latch_.count_down(); }
    void wait() const noexcept { latch_.wait(); }
};

// A special task type that signals the event upon completion.
// Crucially, it uses suspend_always at final_suspend so the frame
// stays alive until the wrapper is destroyed.
template <typename T>
struct SyncWaitTask
{
    struct promise_type
    {
        SyncWaitEvent* event{nullptr};
        std::variant<std::monostate, T, std::exception_ptr> result;

        SyncWaitTask get_return_object() noexcept
        {
            return SyncWaitTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                // Signal the main thread that result is ready.
                // The frame remains alive because we return void/suspend.
                h.promise().event->set();
            }
            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }

        void return_value(T value) { result.template emplace<1>(std::move(value)); }
        void unhandled_exception() noexcept { result.template emplace<2>(std::current_exception()); }
    };

    std::coroutine_handle<promise_type> h_;

    explicit SyncWaitTask(std::coroutine_handle<promise_type> h) : h_(h) {}
    ~SyncWaitTask()
    {
        if (h_)
            h_.destroy();
    }

    void start(SyncWaitEvent& event)
    {
        h_.promise().event = &event;
        h_.resume();
    }

    T get_result()
    {
        if (h_.promise().result.index() == 2)
            std::rethrow_exception(std::get<2>(h_.promise().result));
        return std::get<1>(std::move(h_.promise().result));
    }
};

// Specialization for void
template <>
struct SyncWaitTask<void>
{
    struct promise_type
    {
        SyncWaitEvent* event{nullptr};
        std::exception_ptr exception{nullptr};

        SyncWaitTask get_return_object() noexcept
        {
            return SyncWaitTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept { h.promise().event->set(); }
            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> h_;

    explicit SyncWaitTask(std::coroutine_handle<promise_type> h) : h_(h) {}
    ~SyncWaitTask()
    {
        if (h_)
            h_.destroy();
    }

    void start(SyncWaitEvent& event)
    {
        h_.promise().event = &event;
        h_.resume();
    }

    void get_result()
    {
        if (h_.promise().exception)
            std::rethrow_exception(h_.promise().exception);
    }
};

// Adapter to wrap a user's Task<T> into a SyncWaitTask<T>
template <typename T>
SyncWaitTask<T> MakeSyncTask(Task<T>&& task)
{
    if constexpr (std::is_void_v<T>)
    {
        co_await task;
    }
    else
    {
        co_return co_await task;
    }
}

// The public API
template <typename T>
T SyncWait(Task<T> task)
{
    SyncWaitEvent event;
    auto wrapper = MakeSyncTask(std::move(task));
    wrapper.start(event);
    event.wait();  // Block until worker is done

    // wrapper is destroyed here, cleaning up the frame SAFELY
    if constexpr (!std::is_void_v<T>)
        return wrapper.get_result();
    else
        wrapper.get_result();
}

}  // namespace kio::testing

#endif  // KIO_CORE_TEST_HELPERS_H