#ifndef SYNC_WAIT_H
#define SYNC_WAIT_H

#include <latch>

#include "kio/core/coro.h"

namespace kio::internal
{
class SyncWaitEvent
{
    std::latch latch_{1};

public:
    void set() noexcept { latch_.count_down(); }
    void wait() const noexcept { latch_.wait(); }
};

// Wrapper task that stores result and notifies on completion
template<typename T>
struct sync_wait_task
{
    struct promise_type
    {
        SyncWaitEvent* event{nullptr};
        std::variant<std::monostate, T, std::exception_ptr> storage;

        sync_wait_task get_return_object() noexcept
        {
            return sync_wait_task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }  // NOLINT

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }  // NOLINT
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                h.promise().event->set();  // Signal completion
            }
            void await_resume() noexcept {}  // NOLINT
        };

        final_awaiter final_suspend() noexcept { return {}; }  // NOLINT

        void return_value(T value)
            requires(!std::is_void_v<T>)
        {
            storage = std::move(value);
        }

        void unhandled_exception() noexcept { storage = std::current_exception(); }

        T result()
        {
            if (storage.index() == 2)
            {
                std::rethrow_exception(std::get<2>(storage));
            }
            return std::get<1>(std::move(storage));
        }
    };

    std::coroutine_handle<promise_type> h_;

    explicit sync_wait_task(std::coroutine_handle<promise_type> h) : h_(h) {}

    sync_wait_task(sync_wait_task&& other) noexcept : h_(std::exchange(other.h_, {})) {}

    ~sync_wait_task()
    {
        if (h_) h_.destroy();
    }

    void start(SyncWaitEvent& event)
    {
        h_.promise().event = &event;
        // Start execution
        h_.resume();
    }

    T result() { return h_.promise().result(); }
};

// Specialization for void
template<>
struct sync_wait_task<void>
{
    struct promise_type
    {
        SyncWaitEvent* event{nullptr};
        std::exception_ptr exception;

        sync_wait_task get_return_object() noexcept
        {
            return sync_wait_task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }  // NOLINT

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }  // NOLINT
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept { h.promise().event->set(); }  // NOLINT
            void await_resume() noexcept {}  // NOLINT
        };

        final_awaiter final_suspend() noexcept { return {}; }  // NOLINT

        void return_void() noexcept {}  // NOLINT

        void unhandled_exception() noexcept { exception = std::current_exception(); }

        void result() const
        {
            if (exception)
            {
                std::rethrow_exception(exception);
            }
        }
    };

    std::coroutine_handle<promise_type> h_;

    explicit sync_wait_task(std::coroutine_handle<promise_type> h) : h_(h) {}

    sync_wait_task(sync_wait_task&& other) noexcept : h_(std::exchange(other.h_, {})) {}

    ~sync_wait_task()
    {
        if (h_) h_.destroy();
    }

    void start(SyncWaitEvent& event) const
    {
        h_.promise().event = &event;
        h_.resume();
    }

    void result() const { h_.promise().result(); }
};

template<typename T>
auto MakeSyncAwaitTask(Task<T>&& task) -> sync_wait_task<T>
{
    if constexpr (std::is_void_v<T>)
    {
        co_await std::forward<Task<T>>(task);
        co_return;
    }
    else
    {
        co_return co_await std::forward<Task<T>>(task);
    }
}

}  // namespace kio::internal

namespace kio
{
template<typename T>
auto SyncWait(Task<T> task) -> decltype(auto)
{
    internal::SyncWaitEvent event;
    auto wrapper = internal::MakeSyncAwaitTask(std::move(task));
    wrapper.start(event);
    event.wait();
    return wrapper.result();
}
}  // namespace kio

#endif  // SYNC_WAIT_H
