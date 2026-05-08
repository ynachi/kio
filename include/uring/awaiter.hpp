#pragma once
#include "context.h"

#include <cassert>
#include <concepts>
#include <utility>

#include <liburing.h>

#include "error.hpp"

namespace URing
{
namespace detail
{
struct ResumeCount
{
    Result<std::uint32_t> operator()(const int32_t res) const noexcept
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return static_cast<std::uint32_t>(res);
    }
};

struct ResumeVoid
{
    Result<void> operator()(const int32_t res) const noexcept
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return {};
    }
};
}  // namespace detail

template <typename SetupFunc, typename CompletionFunc = detail::ResumeCount>
    requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<CompletionFunc, int32_t>
class IoAwaiter
{
    IoContext* ctx_;
    Token token_ = {};
    bool completed_ = false;
    [[no_unique_address]] SetupFunc setup_;
    [[no_unique_address]] CompletionFunc complete_;

public:
    IoAwaiter(IoContext& ctx, SetupFunc setup, CompletionFunc complete = {})
        : ctx_(&ctx), setup_(std::move(setup)), complete_(std::move(complete))
    {
        // Allocate token early so it can be safely cancelled if destroyed before suspension
        token_ = ctx_->allocate_token();
    }

    IoAwaiter(IoAwaiter&& other) noexcept
        : ctx_(std::exchange(other.ctx_, nullptr)),
          token_(other.token_),
          completed_(other.completed_),
          setup_(std::move(other.setup_)),
          complete_(std::move(other.complete_))
    {
    }

    IoAwaiter& operator=(IoAwaiter&& other) = delete;
    IoAwaiter(const IoAwaiter&) = delete;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        assert(ctx_ != nullptr && "Attempted to suspend moved-from Awaiter");
        ctx_->get_state(token_).coro_handle = handle;
        ctx_->get_state(token_).is_abandoned = false;

        // THE FIX: Grab SQE and configure it NOW.
        // Any captured variables in setup_ are now mathematically guaranteed
        // to have a stable memory address inside the coroutine frame.
        io_uring_sqe* sqe = ctx_->get_sqe_safe();
        setup_(sqe);
        io_uring_sqe_set_data64(sqe, token_.to_u64());
    }

    auto await_resume() noexcept(noexcept(std::declval<CompletionFunc&>()(std::declval<int32_t>())))
    {
        assert(ctx_ != nullptr && "Attempted to resume moved-from Awaiter");
        completed_ = true;
        const int32_t res = ctx_->get_state(token_).cqe_res;
        ctx_->free_token(token_);
        return complete_(res);
    }

    ~IoAwaiter()
    {
        if (ctx_ != nullptr && !completed_)
        {
            ctx_->get_state(token_).is_abandoned = true;
            ctx_->submit_cancel(token_);
        }
    }
};
}  // namespace URing