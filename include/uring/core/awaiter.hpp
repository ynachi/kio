#pragma once

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <liburing.h>

#include "../error.hpp"
#include "../logger.hpp"

namespace URing
{
class IO;

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
    IO& io_;
    [[no_unique_address]] SetupFunc setup_;
    [[no_unique_address]] MapperFunc mapper_;

public:
    IoAwaiter(IO& io, SetupFunc setup, MapperFunc mapper)
        : io_(io), setup_(std::move(setup)), mapper_(std::move(mapper))
    {
    }

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
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept;

    Result<T> await_resume() noexcept { return mapper_(ops_.res); }
};

// Implementation is moved to context.h after IO is fully defined


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
