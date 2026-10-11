#pragma once
#include <chrono>
#include <coroutine>
#include <concepts>
#include <liburing.h>
#include <optional>
#include <linux/time_types.h>

#include "kio/error.hpp"

namespace kio::io
{
    class Context;

    inline constexpr std::uint64_t IGNORE_TAG = 1;

    enum class IoAwaitableState : uint8_t
    {
        Created,
        Inflight,
        Done,
        Canceled,
    };

    struct IoAwaitableBase
    {
        std::coroutine_handle<> handle{};
        int res{};
        // TODO: maybe do have an enum for the state
        IoAwaitableState state{IoAwaitableState::Created};

        void set_handle(const std::coroutine_handle<> h) noexcept { handle = h; }

        void switch_state(const IoAwaitableState val) noexcept
        {
            // TODO: later check for state transitions, return an error or set a coroutine error
            this->state = val;
        }
    };

    static_assert(alignof(IoAwaitableBase) >= 2, "Op address low bit must be free for tagging");

    template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep>
    class IoAwaitable : public IoAwaitableBase
    {
    public:
        IoAwaitable(Context& io, Prep p, const bool etime_is_success) noexcept : io_(io), prep_(p),
            etime_is_success_(etime_is_success)
        {
        }

        IoAwaitable(const IoAwaitable&) = delete;
        IoAwaitable& operator=(const IoAwaitable&) = delete;

        //TODO: should I delete these ?
        IoAwaitable(IoAwaitable&&) = delete;
        IoAwaitable& operator=(IoAwaitable&&) = delete;

        IoAwaitable& timeout(const std::chrono::nanoseconds duration_ns) noexcept
        {
            const auto s = std::chrono::duration_cast<std::chrono::seconds>(duration_ns);
            link_ts_ = __kernel_timespec{
                .tv_sec = static_cast<long long>(s.count()),
                .tv_nsec = static_cast<long long>((duration_ns - s).count())
            };
            return *this;
        }

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) noexcept;

        Result<int> await_resume() const noexcept
        {
            if (res >= 0)
            {
                return res;
            }
            // TODO: we pay for this if for each non time based ops. Something to do here ?
            if (res == -ETIME && etime_is_success_)
            {
                return 0;
            }
            if (res == -ECANCELED && link_ts_ && state != IoAwaitableState::Canceled)
            {
                return Error::fail_errno(ETIMEDOUT, "coroutine canceled");
            }
            // TODO: if we give a name to the operation, we can use it here. For instance write, read, ...
            // To do this, we could add a member to the awaitable base and set it on construction
            return Error::fail_errno(-res, "io operation failed");
        }

    private:
        Context& io_;
        Prep prep_;
        bool etime_is_success_;
        __kernel_timespec scratch_{};
        std::optional<__kernel_timespec> link_ts_{};
    };
}
