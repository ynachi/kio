#pragma once
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <future>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

#include <liburing.h>

#include "logger.hpp"
#include "operation.hpp"
#include "uring/coro_allocator.hpp"

namespace URing
{
//
// Uring options
//
struct IoOptions
{
    std::uint32_t entries = 16800;
    unsigned flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

    // Sleep after 2 seconds of inactivity
    // Liburing auto wakeup the kernel thread so no need to manually do it
    /// IORING_SETUP_DEFER_TASKRUN is not compatible to SQ_POLL
    /// Also, when SQ_POLL is enabled, make sure to pin work threads and kernel threads
    /// and do them on different CPUs, overwhise, the bench reveals that performance
    /// drops on throughput and latency.
    std::uint32_t sq_thread_idle_ms = 2000;
    // -1 means don't pin to a specific CPU
    int sq_thread_cpu = -1;
};

//
// Forward declaration
//
template <typename T>
struct Task;
class IoWorker;
struct DetachedTask;

class InternalKey
{
    friend class IoContext;
    template <typename T>
    friend struct Task;
    template <typename SetupFunc, typename MapperFunc>
        requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
    friend class IoAwaiter;

    InternalKey() = default;
};

template <typename F>
concept WorkerInitFn = std::invocable<F, IoWorker&> && std::same_as<std::invoke_result_t<F, IoWorker&>, DetachedTask>;

class IoWorker
{
public:
    static constexpr unsigned kUringDefaultFlag =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    static constexpr uint64_t kWakeTag = UINT64_MAX;

private:
    /// TLS IO context
    inline static thread_local IoWorker* tl_io = nullptr;
    //
    // Declare friend structs
    //
    // template <typename T>
    // friend struct Task;
    // friend class IoContext;
    //
    // template <typename SetupFunc, typename MapperFunc>
    //     requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
    // friend class IoAwaiter;

    //
    // Const xprs
    //
    static constexpr size_t kMaxResumesPerTick = 128;
    //
    //  Pools
    //
    OpPool op_pool_;
    std::vector<std::coroutine_handle<>> ready_queue_;
    std::vector<std::coroutine_handle<>> process_queue_;

    io_uring ring_{};
    std::thread::id owner_thread_;
    IoOptions opts_;

    void drain_local();

    void tick() noexcept;

public:
    static IoWorker* current_io(InternalKey) noexcept { return tl_io; }
    explicit IoWorker(InternalKey, const IoOptions& opts = {}) : op_pool_(opts.entries), opts_(opts)
    {
        ready_queue_.reserve(opts.entries);
        process_queue_.reserve(kMaxResumesPerTick);
    }
    IoWorker(const IoWorker&) = delete;
    IoWorker& operator=(const IoWorker&) = delete;
    IoWorker(IoWorker&&) = delete;
    IoWorker& operator=(IoWorker&&) = delete;

    ~IoWorker();

    OpPool& pool(InternalKey) noexcept { return op_pool_; }
    io_uring& ring(InternalKey) noexcept { return ring_; }
    std::vector<std::coroutine_handle<>>& ready_queue(InternalKey) noexcept { return ready_queue_; }

    void init(InternalKey, int wq_fd = -1);
    void run(InternalKey, std::stop_token st) noexcept;
    void request_cancel(InternalKey, uint32_t op_idx) noexcept;

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

class IoContext
{
    std::vector<std::unique_ptr<IoWorker>> contexts_;
    std::vector<std::jthread> workers_;
    std::stop_source stop_source_;
    std::size_t num_threads_;
    IoOptions opts_;
    std::atomic<bool> running_{true};
    InternalKey key_{};

public:
    explicit IoContext(std::size_t num_threads, IoOptions opts = {});

    template <WorkerInitFn InitFn>
    [[nodiscard]] bool start(InitFn&& init_fn)
    {
        if (!running_.load(std::memory_order_acquire))
        {
            ALOG_INFO("IoContext is already running");
            return false;
        }

        // Allocate workers on main thread. No rings created yet
        for (std::size_t i = 0; i < num_threads_; ++i)
        {
            contexts_.emplace_back(new IoWorker(key_, opts_));
        }

        const auto wq_promise = std::make_shared<std::promise<int>>();
        std::shared_future wq_future = wq_promise->get_future();

        for (std::size_t i = 0; i < num_threads_; ++i)
        {
            workers_.emplace_back(
                [this, i, wq_promise, wq_future, init_fn = std::forward<InitFn>(init_fn)]() mutable
                {
                    // TODO: make this configurable Skip CPU 0 for pinning, it SHOULD used for SQPOLL
                    IoWorker::pin_to_cpu(static_cast<int>(i + 1));
                    // init worker 0 as the ring owner
                    if (i == 0)
                    {
                        contexts_[i]->init(key_, -1);
                        // share the ring fd to the others
                        wq_promise->set_value(contexts_[i]->ring(key_).ring_fd);
                    }
                    else
                    {
                        // Threads 1..N: Wait for Thread 0 to finish initialization
                        const int owner_fd = wq_future.get();

                        // Initialize secondary rings attached to Thread 0's WQ
                        contexts_[i]->init(key_, owner_fd);
                    }

                    // Start the user-defined root task
                    init_fn(*contexts_[i]);

                    contexts_[i]->run(key_, stop_source_.get_token());
                });
        }

        // TODO: add a start latch. We need to make sure everything is ok before we return should we ?
        return true;
    }

    bool stop() const { return stop_source_.request_stop(); }

    std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

    // Join threads (explicitly or via destructor)
    void join() { workers_.clear(); }
};
}  // namespace URing
