#pragma once
#include "promise_base.hpp"

namespace URing::detail
{
template <typename T>
struct TaskAwaiter
{
    using Handle = std::coroutine_handle<TaskPromise<T>>;
    Handle handle;

    bool await_ready() const noexcept { return handle.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
    {
        handle.promise().continuation = caller;
        return handle;
    }

    Result<T> await_resume() noexcept
    {
        auto& p = handle.promise();
        if (!p.result.has_value()) [[unlikely]]
        {
            return std::unexpected(make_error_code(ECANCELED));
        }
        return std::move(*p.result);
    }
};
}  // namespace URing::detail