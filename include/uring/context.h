#pragma once
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

#include <liburing.h>

#include "awaiter.hpp"
#include "fd.hpp"
#include "logger.hpp"
#include "mpsc_queue.hpp"
#include "task.hpp"

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

    bool pin_io_worker{false};

    /// max resume per tick
    std::size_t batch_max_size = 128;

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

// ============================================================================
// io_uring C++20 IoWorker
//
// Design decisions:
//   - Share-nothing: each IoThread owns its ring, queue, and allocator
//   - TransferTo is the ONLY cross-thread mechanism
//   - MPSC queue holds raw coroutine_handle<> (8 bytes, no type erasure)
//   - mimalloc linked globally — no custom operator new needed in Task
//   - h.resume() is safe because handles are only enqueued while suspended
//   - Symmetric transfer used inside Task to keep final resume stack-flat
//   - Exceptions: std::expected is the preferred error management mechanism (except during critical resources
//   initialization)
//   - Explicit orchestration
//   IO io0();
//   IO io1(..,io0.ring_fd())
// ============================================================================
class IO
{
public:
    static constexpr unsigned kUringDefaultFlag =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    static constexpr uint64_t kWakeTag = UINT64_MAX;

    explicit IO(const size_t id, std::stop_token st, const int shared_wq_fd = -1, const IoOptions& opts = {})
        : stop_token_(st), opts_(opts), id_(id)
    {
        local_tasks_.reserve(opts_.entries);
        current_batch.reserve(kMaxResumesPerTick);

        // init
        init(shared_wq_fd);

        // start loop
        thread_ = std::jthread(
            [this, id]
            {
                io_uring_register(ring_.ring_fd, IORING_REGISTER_ENABLE_RINGS, nullptr, 0);
                if (opts_.pin_io_worker)
                {
                    pin_to_cpu(static_cast<int>(id));
                }
                this->run(opts_.batch_max_size, stop_token_);
            });
    }
    IO(const IO&) = delete;
    IO& operator=(const IO&) = delete;
    IO(IO&&) = delete;
    IO& operator=(IO&&) = delete;
    ~IO();

    template <typename T>
    void schedule(Task<T> task)
    {
        // release transfers ownership
        post(task.release());
    }

    // thread
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

    //
    // IO Methods
    //
    [[nodiscard]] auto accept(Fd& server_fd, SocketAddress& client_addr, const int flags = 0)
    {
        return IoAwaiter(
            *this,
            [raw_fd = server_fd.fd, &client_addr, flags](io_uring_sqe* sqe)
            {
                // Pre-fill the addrlen so the kernel knows the max buffer size
                client_addr.addrlen = sizeof(sockaddr_storage);
                io_uring_prep_accept(sqe, raw_fd, client_addr.GetMutable(), &client_addr.addrlen, flags);
            },
            [](const int32_t res) -> Result<Fd>
            {
                if (res < 0)
                    return std::unexpected(MakeErrorCode(res));
                // Wrap the newly accepted raw FD into our RAII struct immediately
                return Fd{res};
            });
    }

    /// @brief Accepts a new connection without capturing the client's address
    /// Accept default flag is set to SOCK_NONBLOCK | SOCK_CLOEXEC
    [[nodiscard]] auto accept(Fd& server_fd, const int flags = SOCK_NONBLOCK | SOCK_CLOEXEC)
    {
        return IoAwaiter(
            *this, [raw_fd = server_fd.fd, flags](io_uring_sqe* sqe)
            { io_uring_prep_accept(sqe, raw_fd, nullptr, nullptr, flags); },
            [](const int32_t res) -> Result<Fd>
            {
                if (res < 0)
                {
                    return std::unexpected(MakeErrorCode(res));
                }
                return Fd{res};
            });
    }

    /// Unified Read (offset = -1 tells io_uring to use the current file offset)
    ///
    /// @warning The buffer pointed to by 'buf' MUST remain valid until the operation completes.
    /// Do NOT pass a span to a temporary container (e.g., read(fd, std::vector<byte>(1024))).
    [[nodiscard]] auto read(Fd& fd, std::span<std::byte> buf, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
            { io_uring_prep_read(sqe, raw_fd, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
    }

    // Unified Write
    ///
    /// @warning The buffer pointed to by 'buf' MUST remain valid until the operation completes.
    [[nodiscard]] auto write(Fd& fd, std::span<const std::byte> buf, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
            { io_uring_prep_write(sqe, raw_fd, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
    }

    /// @warning The iovecs and the buffers they point to MUST remain valid until completion.
    [[nodiscard]] auto writev(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
            { io_uring_prep_writev(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
            detail::ResumeInt{});
    }

    // File Ops
    [[nodiscard]] auto open(std::filesystem::path path, const int flags, const mode_t mode = 0644)
    {
        return IoAwaiter(
            *this,
            [path, flags, mode](io_uring_sqe* sqe) { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); },
            [](const int32_t res) -> Result<Fd>
            {
                if (res < 0)
                {
                    return std::unexpected(MakeErrorCode(res));
                }
                return Fd{res};
            });
    }

    /// Close takes ownership of the FD on purpose.
    /// Internally, it release the FD before performing an async close to avoid the Dtor of Fd to make a sync close.
    [[nodiscard]] auto close(Fd&& fd)
    {
        auto raw_fd = fd.Release();
        return IoAwaiter(
            *this, [raw_fd](io_uring_sqe* sqe) { io_uring_prep_close(sqe, raw_fd); }, detail::ResumeVoid{});
    }

    [[nodiscard]] auto remove(std::filesystem::path path)
    {
        return IoAwaiter(
            *this, [path](io_uring_sqe* sqe) { io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0); },
            detail::ResumeVoid{});
    }

    [[nodiscard]] auto fsync(Fd& fd, const bool full_sync = false)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, full_sync](io_uring_sqe* sqe)
            { io_uring_prep_fsync(sqe, raw_fd, full_sync ? 0u : IORING_FSYNC_DATASYNC); }, detail::ResumeVoid{});
    }

    [[nodiscard]] auto fallocate(Fd& fd, const int mode, const off_t offset, const off_t len)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, mode, offset, len](io_uring_sqe* sqe)
            { io_uring_prep_fallocate(sqe, raw_fd, mode, offset, len); }, detail::ResumeVoid{});
    }

    [[nodiscard]] auto ftruncate(Fd& fd, const off_t len)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, len](io_uring_sqe* sqe) { io_uring_prep_ftruncate(sqe, raw_fd, len); },
            detail::ResumeVoid{});
    }

    [[nodiscard]] auto poll(Fd& fd, const unsigned poll_mask)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, poll_mask](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, raw_fd, poll_mask); },
            detail::ResumeVoid{});
    }

    template <typename Rep, typename Period>
    [[nodiscard]] auto timeout(const std::chrono::duration<Rep, Period> dur)
    {
        return IoAwaiter(
            *this,
            [ts = __kernel_timespec{.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(dur).count(),
                                    .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count() %
                                               1'000'000'000}](io_uring_sqe* sqe) mutable
            { io_uring_prep_timeout(sqe, &ts, 0, 0); },
            [](const int32_t res) -> Result<void>
            {
                if (res == -ETIME || res == 0)
                    return {};
                return std::unexpected(MakeErrorCode(res));
            });
    }

    template <typename Rep, typename Period>
    [[nodiscard]] auto sleep(const std::chrono::duration<Rep, Period> dur)
    {
        return timeout(dur);
    }

    /// @brief Connects to a remote SocketAddress
    [[nodiscard]] auto connect(Fd& fd, const SocketAddress& addr)
    {
        // CRITICAL SAFETY FEATURE:
        // We capture 'addr' by VALUE inside the lambda. Because the lambda is
        // stored inside the IoAwaiter, and the IoAwaiter is pinned in the
        // coroutine frame, the kernel is guaranteed to read from a stable memory address
        // even if the caller's temporary SocketAddress goes out of scope!
        return IoAwaiter(
            *this, [raw_fd = fd.fd, addr](io_uring_sqe* sqe) mutable
            { io_uring_prep_connect(sqe, raw_fd, addr.Get(), addr.addrlen); }, detail::ResumeVoid{});
    }

    /// @warning The iovecs and the buffers they point to MUST remain valid until completion.
    [[nodiscard]] auto readv(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
            { io_uring_prep_readv(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
            detail::ResumeInt{});
    }

private:
    static constexpr uint64_t kWakeupSentinel = 0xDEAD'C0DE'DEAD'C0DEULL;

    //
    // Const xprs
    //
    // Bit-tag for MSG_RING: handles are aligned, so bit 0 is safe for tagging
    static constexpr size_t kMaxResumesPerTick = 128;
    static constexpr size_t kMaxRemoteTasksPerTick = 128;
    static constexpr uint8_t kMaxSqeGetRetry = 3;

    io_uring ring_{};
    int wake_fd_{-1};
    uint64_t wake_value_{0};
    bool wake_read_armed_{false};
    std::stop_token stop_token_;
    std::jthread thread_;
    std::thread::id owner_thread_;
    IoOptions opts_;
    size_t id_;
    std::vector<std::coroutine_handle<>> local_tasks_{};
    // Swap to protect against coroutines safely queueing more local work
    std::vector<std::coroutine_handle<>> current_batch{};
    MpscQueue<std::coroutine_handle<>> queue_{};
    std::atomic<bool> wakeup_pending_{false};

    void arm_wake_read() noexcept;

    void submit_or_wait_for();

    // -------------------------------------------------------------------------
    // tick() — main event loop tick
    //
    // INVARIANT: queue_ ONLY contains handles to suspended coroutines.
    // This is guaranteed by:
    //   • Task::initial_suspend()  → new tasks start suspended
    //   • TransferTo::await_suspend() → returns void, framework suspends
    //   • I/O awaitables           → suspend after arming SQE
    //   • Task::final_suspend()    → symmetric transfer suspends at completion
    //
    // Because of this invariant, h.resume() on a dequeued handle is always
    // valid: the coroutine frame exists, is suspended, and has single
    // ownership by this thread at the moment of resume.
    //
    // Stack behavior:
    //   • Symmetric transfer enables tail-call optimization → O(1) stack
    //     for nested co_await chains.
    //   • Completion without further awaits unwinds normally → safe.
    //
    // --------------------------------------------------------
    void tick(std::size_t batch_max_size) noexcept;

    io_uring& ring() noexcept { return ring_; }
    size_t id() const noexcept { return id_; }
    int ring_fd() const noexcept { return ring_.ring_fd; }
    io_uring_sqe* get_sqe() noexcept;

    void init(int wq_fd = -1);
    void run(std::size_t batch_max_size, std::stop_token st) noexcept;
    void wake() const noexcept;

    // scheule
    void post(const std::coroutine_handle<> h)
    {
        if (is_owner_thread())
        {
            local_tasks_.push_back(h);
        }
        else
        {
            queue_.enqueue(h);
            wake();
        }
    }
};

// //=================================================================================================================
// // IO Context
// //=================================================================================================================
// class IoContext
// {
//     std::stop_source stop_source_;
//     std::size_t num_threads_;
//     IoOptions opts_;
//     std::atomic<bool> running_{false};
//     std::latch start_latch_;
//     std::vector<std::unique_ptr<IO>> contexts_;
//     std::vector<std::jthread> workers_;
//
// public:
//     explicit IoContext(std::size_t num_threads, const IoOptions& opts = {});
//     ~IoContext();
//
//     /// Init fn start in the worker thread and SHOULD not block
//     template <WorkerInitFn InitFn>
//     [[nodiscard]] bool start(InitFn&& init_fn)
//     {
//         if (running_.exchange(true, std::memory_order_acq_rel))
//         {
//             ALOG_INFO("IoContext is already running");
//             return false;
//         }
//
//         // Allocate workers on main thread. No rings created yet
//         for (std::size_t i = 0; i < num_threads_; ++i)
//         {
//             contexts_.emplace_back(new IO(key_, i, opts_));
//         }
//
//         const auto wq_promise = std::make_shared<std::promise<int>>();
//         std::shared_future wq_future = wq_promise->get_future();
//         const auto stop_token = stop_source_.get_token();
//
//         for (std::size_t i = 0; i < num_threads_; ++i)
//         {
//             workers_.emplace_back(
//                 [this, i, wq_promise, wq_future, stop_token, init_fn = std::forward<InitFn>(init_fn)]() mutable
//                 {
//                     // IoWorker::pin_to_cpu(static_cast<int>(i));
//                     // init worker 0 as the ring owner
//                     if (i == 0)
//                     {
//                         contexts_[i]->init(key_, -1);
//                         // share the ring fd to the others
//                         wq_promise->set_value(contexts_[i]->ring(key_).ring_fd);
//                     }
//                     else
//                     {
//                         // Threads 1..N: Wait for Thread 0 to finish initialization
//                         const int owner_fd = wq_future.get();
//
//                         // Initialize secondary rings attached to Thread 0's WQ
//                         contexts_[i]->init(key_, owner_fd);
//                     }
//
//                     start_latch_.count_down();
//                     start_latch_.wait();
//
//                     // Start the user-defined root task
//                     // All Io on this method uses the tls context
//                     init_fn();
//
//                     contexts_[i]->run(key_, opts_.batch_max_size, stop_token);
//                 });
//         }
//
//         return true;
//     }
//
//     bool stop() const { return stop_source_.request_stop(); }
//
//     std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }
//
//     // TODO: expose a safer scheduling mechanism and remove those methods
//     [[nodiscard]] IO& worker(const std::size_t idx) const { return *contexts_[idx]; }
//     [[nodiscard]] std::size_t worker_count() const noexcept { return contexts_.size(); }
//
//     // Join threads (explicitly or via destructor)
//     void join() noexcept
//     {
//         workers_.clear();
//         contexts_.clear();
//         running_.store(false, std::memory_order_release);
//     }
// };
//
// // ============================================================================
// // TransferTo — cross-thread execution transfer
// //
// // Usage: co_await TransferTo{target_thread};
// //
// // Mechanism:
// //   1. await_ready() returns false → always suspend
// //   2. await_suspend() posts handle to target queue, returns void → suspension
// //   3. Target thread drains queue, calls h.resume() → execution resumes on target
// //
// // Stack safety: await_suspend does NOT call resume(). It returns immediately,
// // allowing the caller's stack to unwind. Resume happens later from the target's
// // drain loop, which has a flat stack.
// // ============================================================================
// struct TransferTo
// {
//     IO& target;
//
//     bool await_ready() noexcept { return false; }
//
//     void await_suspend(std::coroutine_handle<> h) noexcept
//     {
//         InternalKey k{};
//         target.post(k, h);
//     }
//
//     void await_resume() noexcept {}
// };
}  // namespace URing
