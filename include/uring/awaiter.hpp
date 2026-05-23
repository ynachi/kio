#pragma once
#include "context.h"

#include <concepts>
#include <cstring>
#include <utility>

#include <liburing.h>

#include "error.hpp"

namespace URing
{

struct IoOps
{
    std::coroutine_handle<> h{std::noop_coroutine()};
    int32_t res{-1};
};

template <typename SetupFunc, typename MapperFunc>
    requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
class IoAwaiter
{
    // Extract the exact Result<T> type returned by the MapperFunc
    using ResultType = std::invoke_result_t<MapperFunc, int32_t>;
    // Extract the T from Result<T> so we can type the coroutine handle
    using T = ResultType::value_type;

    // for friend access
    IoOps ops_{};
    InternalKey key_{};
    [[no_unique_address]] SetupFunc setup_;
    [[no_unique_address]] MapperFunc mapper_;

public:
    IoAwaiter(SetupFunc setup, MapperFunc mapper) : setup_(std::move(setup)), mapper_(std::move(mapper)) {}

    bool await_ready() const noexcept { return false; }

    // IoAwaiter MUST be pinned in the coroutine frame because it captures
    // pointers/references to buffers and metadata that the kernel will
    // access asynchronously. Deleting the move constructor ensures stability
    // and relies on C++17 mandatory copy elision for RVO from I/O functions.
    IoAwaiter(IoAwaiter&& other) noexcept = delete;
    IoAwaiter& operator=(IoAwaiter&& other) = delete;
    IoAwaiter(const IoAwaiter&) = delete;

    ~IoAwaiter() = default;

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
    {
        const auto io = IoWorker::current_io();
        // skip runtime check as io cannot be nil if io context is normally started
        // if not started, nothing could work anyway.
        assert(io != nullptr && "IoAwaiter used outside of an IoWorker thread");

        // prepare sqe
        io_uring_sqe* sqe = io->get_sqe(key_);
        if (sqe == nullptr)
        {
            // This path is now extremely rare thanks to tick-level submit and
            // the on-demand fallback in get_sqe.
            ALOG_WARN("failed to get SQE after submit; completing operation with ENOSPC");
            ops_.res = -ENOSPC;
            return h;
        }

        this->ops_.h = h;
        setup_(sqe);
        io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&ops_));

        return std::noop_coroutine();
    }

    Result<T> await_resume() noexcept { return mapper_(ops_.res); }
};

// Helper for void-returning operations in io.hpp (e.g., detail::ResumeVoid)
namespace detail
{
struct ResumeVoid
{
    Result<void> operator()(const int32_t res) const noexcept
    {
        if (res < 0)
        {
            return std::unexpected(MakeErrorCode(res));
        }

        return {};
    }
};

struct ResumeInt
{
    Result<int32_t> operator()(const int32_t res) const noexcept
    {
        if (res < 0)
        {
            return std::unexpected(MakeErrorCode(res));
        }
        return res;
    }
};
}  // namespace detail
}  // namespace URing
