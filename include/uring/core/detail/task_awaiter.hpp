#pragma once

#include <cassert>

#include "promise_base.hpp"

namespace kio::detail
{
template <typename T>
struct TaskAwaiter
{
    using Handle = std::coroutine_handle<TaskPromise<T>>;
    Handle handle;

    // A moved-from or released Task has a null handle. Calling done() on it is
    // undefined behaviour, so co_await must not suspend into a dead frame.
    bool await_ready() const noexcept { return !handle || handle.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
    {
        handle.promise().continuation = caller;
        return handle;
    }

    Result<T> await_resume() noexcept
    {
        if (!handle) [[unlikely]]
        {
            return kio::Error::fail_errc(std::errc::operation_canceled);
        }

        auto& p = handle.promise();
        assert(!p.result_taken && "Task awaited twice; the Result is single-consume");

        // Defined fallback for release builds, where the assert is compiled out.
        if (p.result_taken || !p.result.has_value()) [[unlikely]]
        {
            return kio::Error::fail_errc(std::errc::operation_canceled);
        }

        p.result_taken = true;
        return std::move(*p.result);
    }
};
}  // namespace kio::detail