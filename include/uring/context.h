#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include <liburing.h>

#ifdef BLOCK_SIZE
    #pragma push_macro("BLOCK_SIZE")
    #undef BLOCK_SIZE
    #define URING_RESTORE_BLOCK_SIZE_MACRO
#endif
#include "libs/concurrentqueue.hpp"
#ifdef URING_RESTORE_BLOCK_SIZE_MACRO
    #pragma pop_macro("BLOCK_SIZE")
    #undef URING_RESTORE_BLOCK_SIZE_MACRO
#endif
#include "operation.hpp"

namespace URing
{
template <typename T>
struct Task;
class IoContext;
struct DetachedTask;

template <class F>
concept RemoteSpawnFn =
    std::invocable<F&, IoContext&> && std::same_as<std::invoke_result_t<F&, IoContext&>, DetachedTask>;

class IoContext
{
public:
    static constexpr unsigned kUringDefaultFlag =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    static constexpr uint64_t kWakeTag = UINT64_MAX;

private:
    template <typename T>
    friend struct Task;

    OpPool op_pool_;
    io_uring ring_{};
    int wake_fd_{-1};
    uint64_t wake_value_{0};
    bool wake_read_armed_{false};

    std::vector<std::coroutine_handle<>> ready_queue_;
    std::vector<std::coroutine_handle<>> process_queue_;
    moodycamel::ConcurrentQueue<std::move_only_function<void(IoContext&)>> spawn_queue_;
    moodycamel::ConsumerToken spawn_consumer_token_;

    static constexpr size_t kMaxResumesPerTick = 128;
    static constexpr size_t kMaxRemoteDrainPerTick = kMaxResumesPerTick;
    std::thread::id owner_thread_;
    std::stop_token stop_token_;

    void request_cancel(uint32_t op_idx) noexcept;

    void tick(std::chrono::nanoseconds timeout_ns) noexcept;

    void arm_wake_read() noexcept;

    void wake() const noexcept;

    void drain_remote_spawns() noexcept;

public:
    explicit IoContext(std::uint32_t entries = 16800, unsigned flags = kUringDefaultFlag);
    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    ~IoContext();

    OpPool& pool() noexcept { return op_pool_; }
    io_uring& ring() noexcept { return ring_; }

    // Templated run accepts any chrono duration, defaults to 10ms
    /// tick_timeout is the time a tick will wait for the kernel IO to arrive
    /// And alternative would have been an eventfd but lets start here for now
    template <typename Rep, typename Period>
    void run(std::stop_token st, std::chrono::duration<Rep, Period> tick_timeout) noexcept
    {
        auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_timeout);
        while (!st.stop_requested())
        {
            tick(timeout_ns);
        }
    }

    template <RemoteSpawnFn F>
    [[nodiscard]] bool spawn(F&& f)
    {
        if (is_owner_thread())
        {
            std::forward<F>(f)(*this);
            return true;
        }
        std::move_only_function<void(IoContext&)> fn{[factory = std::forward<F>(f)](IoContext& ctx) mutable
                                                     { factory(ctx); }};

        if (spawn_queue_.enqueue(std::move(fn)))
        {
            wake();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool is_owner_thread() const noexcept { return owner_thread_ == std::this_thread::get_id(); }
};
}  // namespace URing
