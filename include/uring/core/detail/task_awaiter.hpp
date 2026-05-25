#pragma once
#include "promise_base.hpp"

namespace URing::detail
{
template <typename T>
struct TaskAwaiter
{
    using Handle = std::coroutine_handle<TaskPromise<T>>;
    Handle handle_;

    bool await_ready() const noexcept { return handle_.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
    {
        handle_.promise().continuation = caller;
        return handle_;
    }

    Result<T> await_resume()
    {
        auto& p = handle_.promise();

        if (p.exception)
        {
            std::rethrow_exception(p.exception);
        }

        if (!p.result_.has_value())
        {
            return std::unexpected(MakeErrorCode(ECANCELED));
        }

        return std::move(*p.result_);
    }
};
}  // namespace URing::detail