#pragma once
#include "context.h"

#include <concepts>
#include <cstring>
#include <utility>

#include <liburing.h>

#include "error.hpp"
#include "task.hpp"
#include "tracer.hpp"

namespace URing
{

template <typename SetupFunc, typename MapperFunc>
    requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
class IoAwaiter
{
    // Extract the exact Result<T> type returned by the MapperFunc
    using ResultType = std::invoke_result_t<MapperFunc, int32_t>;
    // Extract the T from Result<T> so we can type the coroutine handle
    using T = ResultType::value_type;

    IoContext& ctx_;
    Token token_ = {};
#if URING_ENABLE_TRACING
    const char* op_name_;
#endif
    [[no_unique_address]] SetupFunc setup_;
    [[no_unique_address]] MapperFunc mapper_;

public:
#if URING_ENABLE_TRACING
    IoAwaiter(IoContext& ctx, const char* op_name, SetupFunc setup, MapperFunc mapper)
        : ctx_(ctx), op_name_(op_name), setup_(std::move(setup)), mapper_(std::move(mapper))
    {
    }
#else
    IoAwaiter(IoContext& ctx, SetupFunc setup, MapperFunc mapper)
        : ctx_(ctx), setup_(std::move(setup)), mapper_(std::move(mapper))
    {
    }
#endif

    bool await_ready() const noexcept { return false; }

    IoAwaiter(IoAwaiter&& other) noexcept = delete;
    IoAwaiter& operator=(IoAwaiter&& other) = delete;
    IoAwaiter(const IoAwaiter&) = delete;

    ~IoAwaiter() = default;

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) noexcept
    {
        auto* ring = &ctx_.ring();
        token_ = ctx_.pool().allocate(h);
        auto op = ctx_.pool().try_get(token_);
#if URING_ENABLE_TRACING
        op->op_name = op_name_;
#endif

        // prepare sqe
        io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (sqe == nullptr)
        {
            const int ret = io_uring_submit(ring);
#if URING_ENABLE_TRACING
            Tracer::sqe_slow(token_, ret, op_name_);
#endif
            if (ret < 0)
            {
                ALOG_WARN("io_uring_submit failed while trying to free SQE space: {}", std::strerror(-ret));
            }

            sqe = io_uring_get_sqe(ring);
            if (sqe == nullptr)
            {
#if URING_ENABLE_TRACING
                Tracer::sqe_full(token_, op_name_);
#endif
                ALOG_WARN("failed to get SQE after submit; completing operation with ENOSPC");
                op->result_code = -ENOSPC;
                h.resume();
                return;
            }
        }
        else
        {
#if URING_ENABLE_TRACING
            Tracer::sqe_fast(token_, op_name_);
#endif
        }

        setup_(sqe);
        op->original_ud = token_.pack();
        io_uring_sqe_set_data64(sqe, token_.pack());
#if URING_ENABLE_TRACING
        Tracer::submit(token_, op_name_);
#endif

        if constexpr (requires(Promise& p) {
                          p.pending_op_idx_;
                          p.ctx_;
                      })
        {
            h.promise().pending_op_idx_ = token_.idx;
            h.promise().ctx_ = &ctx_;
        }
    }

    Result<T> await_resume()
    {
        // We just woke up! The event loop populated the result_code.
        auto& op = ctx_.pool().get(token_.idx);
        int result = op.result_code;

        // FREE THE SLOT IMMEDIATELY!
        // We have our data, the kernel is done, we don't need the slot anymore.
        ctx_.pool().deallocate(token_);

        return mapper_(result);
    }
};

// Helper for void-returning operations in io.hpp (e.g., detail::ResumeVoid)
namespace detail
{
struct ResumeVoid
{
    Result<void> operator()(const int32_t res) const noexcept
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));

        return {};
    }
};

struct ResumeInt
{
    Result<int32_t> operator()(const int32_t res) const noexcept
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        // std::expected will naturally wrap this integer into a success state
        return res;
    }
};
}  // namespace detail
}  // namespace URing
