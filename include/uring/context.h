#pragma once
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <future>
#include <latch>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

#include <liburing.h>

#include "logger.hpp"
#include "operation.hpp"
#include "task.hpp"
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

    std::uint32_t tick_timeout_ms = 10;

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

// io_uring::user_data is shared by normal I/O completions and control messages.
// The top two bits are reserved as a tag field:
//   00: normal OpPool Token, encoded by Token::pack()
//   01: MSG_RING delivered a RemoteTask handle to the target worker
//   10: source-side MSG_RING completion for delivery failure handling
// Normal tokens must keep these bits clear; see Token::kMaxGeneration.
static constexpr uint64_t kRemoteTagMask = 0xC000000000000000ULL;
static constexpr uint64_t kRemoteTaskTag = 0x4000000000000000ULL;
static constexpr uint64_t kRemoteSendTag = 0x8000000000000000ULL;
//
// Forward declaration
//
class IoWorker;
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
concept WorkerInitFn = std::invocable<F> && std::same_as<std::invoke_result_t<F>, void>;

template <typename F, typename Worker = IoWorker>
concept RemoteFactory = std::invocable<F&&> &&
                        // Must return exactly RemoteTask (no Task<T>, no DetachedTask)
                        std::same_as<std::invoke_result_t<F&&>, RemoteTask> &&
                        // Must be trivially move-constructible: no complex captures
                        std::is_trivially_move_constructible_v<std::remove_reference_t<F>> &&
                        // Must be trivially destructible: no custom cleanup across threads
                        std::is_trivially_destructible_v<std::remove_reference_t<F>>;

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
    // Const xprs
    //
    // Bit-tag for MSG_RING: handles are aligned, so bit 0 is safe for tagging
    static constexpr size_t kMaxResumesPerTick = 128;
    static constexpr uint8_t kMaxSqeGetRetry = 3;
    //
    //  Pools
    //
    OpPool op_pool_;
    std::vector<std::coroutine_handle<>> ready_queue_;
    std::vector<std::coroutine_handle<>> process_queue_;

    io_uring ring_{};
    std::thread::id owner_thread_;
    IoOptions opts_;
    size_t id_;

    void drain_local();

    void tick() noexcept;

public:
    static IoWorker* current_io() noexcept { return tl_io; }
    explicit IoWorker(InternalKey, size_t id, const IoOptions& opts = {}) : op_pool_(opts.entries), opts_(opts), id_(id)
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

    size_t id() const noexcept { return id_; }

    io_uring_sqe* get_sqe(InternalKey) noexcept
    {
        io_uring_sqe* sqe = nullptr;

        for (auto i = 0; i < kMaxSqeGetRetry; ++i)
        {
            sqe = io_uring_get_sqe(&ring_);
            if (sqe != nullptr)
                break;
            io_uring_submit(&ring_);
        }
        assert(sqe != nullptr && "Sqe null after 3 retries, this is a fatal error");
        return sqe;
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

//=================================================================================================================
// IO Context
//=================================================================================================================
class IoContext
{
    std::stop_source stop_source_;
    std::size_t num_threads_;
    IoOptions opts_;
    std::atomic<bool> running_{false};
    std::latch start_latch_;
    InternalKey key_{};
    std::vector<std::unique_ptr<IoWorker>> contexts_;
    std::vector<std::jthread> workers_;

public:
    explicit IoContext(std::size_t num_threads, const IoOptions& opts = {});
    ~IoContext();

    /// Init fn start in the worker thread and SHOULD not block
    template <WorkerInitFn InitFn>
    [[nodiscard]] bool start(InitFn&& init_fn)
    {
        if (running_.exchange(true, std::memory_order_acq_rel))
        {
            ALOG_INFO("IoContext is already running");
            return false;
        }

        // Allocate workers on main thread. No rings created yet
        for (std::size_t i = 0; i < num_threads_; ++i)
        {
            contexts_.emplace_back(new IoWorker(key_, i, opts_));
        }

        const auto wq_promise = std::make_shared<std::promise<int>>();
        std::shared_future wq_future = wq_promise->get_future();
        const auto stop_token = stop_source_.get_token();

        for (std::size_t i = 0; i < num_threads_; ++i)
        {
            workers_.emplace_back(
                [this, i, wq_promise, wq_future, stop_token, init_fn = std::forward<InitFn>(init_fn)]() mutable
                {
                    start_latch_.count_down();

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
                    // All Io on this method uses the tls context
                    init_fn();

                    contexts_[i]->run(key_, stop_token);
                });
        }

        start_latch_.wait();
        // TODO: add a start latch. We need to make sure everything is ok before we return should we ?
        return true;
    }

    /// Dispatch to uses more system call to send coroutines handles arround
    /// It should not be used the default scheduling mechanism
    template <typename RemoteFactory>
    void dispatch_to(IoWorker& target_io, RemoteFactory&& fn)
    {
        RemoteTask task = fn();
        auto handle = task.release();

        auto current_io = IoWorker::current_io();

        // if we are on target thread, do not ring msg
        if (current_io == &target_io)
        {
            ALOG_DEBUG("calling dispatch_to on own thread is discouraged, use a detached task instead");
            target_io.ready_queue(key_).push_back(handle);
            return;
        }

        assert(current_io != nullptr && "dispatch_to called from outside IoWorker::run()");

        io_uring_sqe* sqe = current_io->get_sqe(key_);
        if (sqe == nullptr)
        {
            // can't send, destroy
            ALOG_ERROR("failed to send coroutine to target worker, destroying it");
            handle.destroy();
        }

        // Encode the handle for the target thread (Tag = 1)
        const uint64_t encoded_target =
            (reinterpret_cast<uint64_t>(handle.address()) & ~kRemoteTagMask) | kRemoteTaskTag;

        io_uring_prep_msg_ring(sqe, target_io.ring(key_).ring_fd, 0, encoded_target, 0);

        // Encode the handle for the SENDER'S completion queue (Tag = 2)
        // This allows the sender to track if the delivery failed.
        const uint64_t encoded_sender =
            (reinterpret_cast<uint64_t>(handle.address()) & ~kRemoteTagMask) | kRemoteSendTag;

        io_uring_sqe_set_data64(sqe, encoded_sender);

        io_uring_submit(&current_io->ring(key_));
    }

    bool stop() const { return stop_source_.request_stop(); }

    std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

    // TODO: expose a safer scheduling mechanism and remove those methods
    [[nodiscard]] IoWorker& worker(const std::size_t idx) const { return *contexts_[idx]; }
    [[nodiscard]] std::size_t worker_count() const noexcept { return contexts_.size(); }

    // Join threads (explicitly or via destructor)
    void join()
    {
        workers_.clear();
        running_.store(false, std::memory_order_release);
    }
};
}  // namespace URing
