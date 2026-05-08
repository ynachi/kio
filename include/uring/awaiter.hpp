#pragma once
#include "context.h"

#include <cassert>
#include <concepts>
#include <utility>

#include <liburing.h>

#include "error.hpp"
namespace URing
{
template <typename SetupFunc>
    requires std::invocable<SetupFunc, io_uring_sqe*>
class IoAwaiter
{
    IoContext* ctx_;
    Token token_ = {};
    bool completed_ = {};

public:
    IoAwaiter(IoContext& ctx, SetupFunc setup) : ctx_(&ctx)
    {
        token_ = ctx.allocate_token();
        io_uring_sqe* sqe = ctx.get_sqe();

        // 1. Execute the user-provided prep logic (Resolved at compile time!)
        setup(sqe);

        // 2. Safely attach our Generational Token
        io_uring_sqe_set_data64(sqe, token_.to_u64());
    }

    IoAwaiter(IoAwaiter&& other) noexcept
        : ctx_(std::exchange(other.ctx_, nullptr)), token_(other.token_), completed_(other.completed_)
    {
    }
    
    IoAwaiter& operator=(IoAwaiter&& other) noexcept = delete;
    IoAwaiter(const IoAwaiter&) = delete;
    IoAwaiter& operator=(const IoAwaiter&) = delete;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        assert(ctx_ != nullptr && "Attempted to suspend on a moved-from Awaiter");
        // This allows the optimizer to remove any null-checks inside get_sqe()
        [[assume(ctx_ != nullptr)]];
        ctx_->get_state(token_).coro_handle = handle;
        ctx_->get_state(token_).is_abandoned = false;
    }

    Result<std::uint32_t> await_resume() noexcept
    {
        assert(ctx_ != nullptr && "Attempted to resume on a moved-from Awaiter");
        // This allows the optimizer to remove any null-checks inside get_sqe()
        [[assume(ctx_ != nullptr)]];

        completed_ = true;
        const int32_t res = ctx_->get_state(token_).cqe_res;
        ctx_->free_token(token_);

        if (res < 0)
        {
            return std::unexpected(std::make_error_code(static_cast<std::errc>(-res)));
        }
        return static_cast<uint32_t>(res);
    }

    ~IoAwaiter()
    {
        if (ctx_ == nullptr)
        {
            return;
        }

        if (!completed_)
        {
            ctx_->get_state(token_).is_abandoned = true;
            ctx_->submit_cancel(token_);
        }
    }
};
}  // namespace URing