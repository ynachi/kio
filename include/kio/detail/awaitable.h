#pragma once
#include "absl/log/check.h"

#include "kio/error.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstdint>

#include <liburing.h>

#include <linux/time_types.h>

namespace kio::io
{
    class Context;

    inline constexpr std::uint64_t IGNORE_TAG = 1;

    // Lifecycle only, and terminal: Created -> Inflight -> Done. Cancel intent
    // is a separate flag bit, NOT a lifecycle state.  Canceled-as-a-state
    // conflated "cancel requested, CQE pending" with "CQE consumed", which made
    // reap()'s double-CQE CHECK accept re-reaped canceled ops and forced a
    // special case to avoid writing Done over Canceled.
    enum class IoAwaitableState : uint8_t
    {
        Created,
        Inflight,
        Done,
    };

    // The state byte packs the lifecycle (low bits) and per-op flags (high
    // bits) so an in-flight op costs no extra bytes for configuration
    inline constexpr uint8_t kLifecycleMask = 0x07;
    inline constexpr uint8_t kLifecycleMaskInv = static_cast<uint8_t>(~kLifecycleMask); // 0xF8
    static_assert(static_cast<uint8_t>(IoAwaitableState::Done) <= kLifecycleMask);
    inline constexpr uint8_t kEtimeIsSuccessBit = 0x40; // -ETIME completion is a success value
    inline constexpr uint8_t kHasLinkTimeoutBit = 0x80; // link_ts_ holds a timeout
    inline constexpr uint8_t kCancelRequestedBit = 0x20;
    inline constexpr uint8_t kPreFailedBit = 0x10;

    struct IoAwaitableBase
    {
        std::coroutine_handle<> handle{};
        int res{};
        // Lifecycle in the low bits; kEtimeIsSuccessBit / kHasLinkTimeoutBit /
        // kCancelRequestedBit above.
        uint8_t state{static_cast<uint8_t>(IoAwaitableState::Created)};

        [[nodiscard]] IoAwaitableState lifecycle() const noexcept
        {
            return static_cast<IoAwaitableState>(state & kLifecycleMask);
        }

        [[nodiscard]] bool etime_is_success() const noexcept { return state & kEtimeIsSuccessBit; }
        [[nodiscard]] bool has_link_timeout() const noexcept { return state & kHasLinkTimeoutBit; }
        void set_has_link_timeout() noexcept { state |= kHasLinkTimeoutBit; }
        [[nodiscard]] bool cancel_requested() const noexcept { return state & kCancelRequestedBit; }
        [[nodiscard]] bool pre_failed() const noexcept { return state & kPreFailedBit; }
        void set_cancel_requested() noexcept { state |= kCancelRequestedBit; }

        // Masked write so the flag bits survive. The legal edges are
        // Created -> Inflight (await_suspend) and Inflight -> Done (reap);
        // each of those two call sites CHECKs its source lifecycle.
        void set_lifecycle(const IoAwaitableState val) noexcept
        {
            state = (state & kLifecycleMaskInv) | static_cast<uint8_t>(val);
        }
    };

    static_assert(alignof(IoAwaitableBase) >= 2, "Op address low bit must be free for tagging");

    template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep>
    class IoAwaitable : public IoAwaitableBase
    {
    public:
        IoAwaitable(Context& io, Prep p, const bool etime_is_success, const int pre_errno = 0) noexcept
            : io_(io), prep_(std::move(p))
        {
            if (etime_is_success)
            {
                state |= kEtimeIsSuccessBit;
            }
            if (pre_errno != 0)
            {
                res = -pre_errno;
                state |= kPreFailedBit;
            }
        }

        IoAwaitable(const IoAwaitable&) = delete;
        IoAwaitable& operator=(const IoAwaitable&) = delete;
        IoAwaitable(IoAwaitable&&) = delete;
        IoAwaitable& operator=(IoAwaitable&&) = delete;

        // Defense in depth for coroutine types kio does not own: Task
        // CHECK-aborts before destroying a suspended frame, but a hand-rolled
        // coroutine or a raw handle.destroy() could kill an awaiter whose
        // CQE is still in flight. reap() would cast a dangling user_data.
        ~IoAwaitable()
        {
            CHECK(lifecycle() != IoAwaitableState::Inflight) << "IoAwaitable destroyed in flight";
        }

        IoAwaitable& timeout(const std::chrono::nanoseconds duration_ns) noexcept
        {
            const auto d = std::max(duration_ns, std::chrono::nanoseconds{});
            const auto s = std::chrono::duration_cast<std::chrono::seconds>(d);
            link_ts_ = __kernel_timespec{
                .tv_sec = static_cast<long long>(s.count()),
                .tv_nsec = static_cast<long long>((d - s).count())
            };
            set_has_link_timeout();
            return *this;
        }

        bool await_ready() const noexcept { return pre_failed(); }

        void await_suspend(std::coroutine_handle<> h) noexcept;

        // Defined in context.h: needs the complete Context (canceling_all_).
        Result<int> await_resume() const noexcept;

    private:
        Context& io_;
        [[no_unique_address]] Prep prep_;
        // Scratch for the value prep_ hands to the kernel (sleep durations)
        // and the timespec of a chained .timeout() (link_ts_). They are
        // deliberately SEPARATE members: a sleep CAN also carry a .timeout()
        // (timing_cancel_test pins that combination), so the two uses do
        // coexist and a union would corrupt one of them. Both must live in
        // the frame: the kernel reads them at submit time, after
        // await_suspend has returned.
        __kernel_timespec scratch_{};
        __kernel_timespec link_ts_{};
    };
} // namespace kio::io
