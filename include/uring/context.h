#pragma once
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <system_error>
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
#include "logger.hpp"
#include "operation.hpp"
#include "tracer.hpp"
#include "uring/coro_allocator.hpp"

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

    void tick() noexcept;

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

    void run(const std::stop_token st) noexcept
    {
        // 128-byte frames: 64 preallocated
        // 256-byte frames: most common
        // 512-byte frames: combinators
        CoroAllocator::prewarm(0, 128);
        CoroAllocator::prewarm(1, 256);
        CoroAllocator::prewarm(2, 64);

        std::stop_callback wake_on_stop{st, [this] { wake(); }};
        while (!st.stop_requested())
        {
            tick();
        }
    }

    template <RemoteSpawnFn F>
    [[nodiscard]] bool spawn(F&& f)
    {
        if (is_owner_thread())
        {
            URING_TRACE_SPAWN_FAST();
            std::forward<F>(f)(*this);
            return true;
        }

        URING_TRACE_SPAWN_SLOW();
        ALOG_DEBUG("Using the external dispatch queue");
        std::move_only_function<void(IoContext&)> fn{[factory = std::forward<F>(f)](IoContext& ctx) mutable
                                                     { factory(ctx); }};

        if (spawn_queue_.enqueue(std::move(fn)))
        {
            wake();
            return true;
        }

        URING_TRACE_SPAWN_FULL();
        ALOG_WARN("failed to enqueue remote spawn");
        return false;
    }

    [[nodiscard]] bool is_owner_thread() const noexcept { return owner_thread_ == std::this_thread::get_id(); }

    static void pin_to_cpu(int cpu_id)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id % static_cast<int>(std::thread::hardware_concurrency()), &cpuset);

        if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
        {
            ALOG_INFO("Warning: Failed to pin to CPU {}: {}", cpu_id, std::generic_category().message(rc));
        }
    }
};
}  // namespace URing
