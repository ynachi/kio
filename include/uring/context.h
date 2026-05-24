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
    friend class IoContext;
    friend struct TransferTo;
    template <typename SetupFunc, typename MapperFunc>
        requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
    friend class IoAwaiter;

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
        post(task.release());
    }

    void join() noexcept
    {
        if (thread_.joinable())
        {
            thread_.join();
        }
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

    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }

    //
    // IO Methods
    //
    [[nodiscard]] auto accept(Fd& server_fd, SocketAddress& client_addr, const int flags = 0)
    {
        return IoAwaiter(
            *this,
            [raw_fd = server_fd.fd, &client_addr, flags](io_uring_sqe* sqe)
            {
                client_addr.addrlen = sizeof(sockaddr_storage);
                io_uring_prep_accept(sqe, raw_fd, client_addr.GetMutable(), &client_addr.addrlen, flags);
            },
            [](const int32_t res) -> Result<Fd>
            {
                if (res < 0)
                    return std::unexpected(MakeErrorCode(res));
                return Fd{res};
            });
    }

    [[nodiscard]] auto accept(Fd& server_fd, const int flags = SOCK_NONBLOCK | SOCK_CLOEXEC)
    {
        return IoAwaiter(
            *this, [raw_fd = server_fd.fd, flags](io_uring_sqe* sqe)
            { io_uring_prep_accept(sqe, raw_fd, nullptr, nullptr, flags); },
            [](const int32_t res) -> Result<Fd>
            {
                if (res < 0)
                    return std::unexpected(MakeErrorCode(res));
                return Fd{res};
            });
    }

    [[nodiscard]] auto read(Fd& fd, std::span<std::byte> buf, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
            { io_uring_prep_read(sqe, raw_fd, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
    }

    [[nodiscard]] auto write(Fd& fd, std::span<const std::byte> buf, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
            { io_uring_prep_write(sqe, raw_fd, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
    }

    [[nodiscard]] auto writev(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
            { io_uring_prep_writev(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
            detail::ResumeInt{});
    }

    [[nodiscard]] auto open(std::filesystem::path path, const int flags, const mode_t mode = 0644)
    {
        return IoAwaiter(
            *this,
            [path, flags, mode](io_uring_sqe* sqe) { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); },
            [](const int32_t res) -> Result<Fd>
            {
                if (res < 0)
                    return std::unexpected(MakeErrorCode(res));
                return Fd{res};
            });
    }

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

    [[nodiscard]] auto connect(Fd& fd, const SocketAddress& addr)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, addr](io_uring_sqe* sqe) mutable
            { io_uring_prep_connect(sqe, raw_fd, addr.Get(), addr.addrlen); }, detail::ResumeVoid{});
    }

    [[nodiscard]] auto readv(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
            { io_uring_prep_readv(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
            detail::ResumeInt{});
    }

private:
    static constexpr uint64_t kWakeupSentinel = 0xDEAD'C0DE'DEAD'C0DEULL;
    static constexpr size_t kMaxResumesPerTick = 128;

    io_uring ring_{};
    int wake_fd_{-1};
    uint64_t wake_value_{0};
    std::stop_token stop_token_;
    std::jthread thread_;
    std::thread::id owner_thread_;
    IoOptions opts_;
    size_t id_;
    std::vector<std::coroutine_handle<>> local_tasks_{};
    std::vector<std::coroutine_handle<>> current_batch{};
    MpscQueue<std::coroutine_handle<>> queue_{};

    void init(int wq_fd = -1);
    void run(std::size_t batch_max_size, std::stop_token st) noexcept;
    void tick(std::size_t batch_max_size) noexcept;
    void submit_or_wait_for();
    void arm_wake_read() noexcept;
    void wake() const noexcept;
    io_uring_sqe* get_sqe() noexcept;

    int ring_fd() const noexcept { return ring_.ring_fd; }

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

//=================================================================================================================
// IO Context
//=================================================================================================================
class IoContext
{
    std::stop_source stop_source_;
    IoOptions opts_;
    std::vector<std::unique_ptr<IO>> workers_;

public:
    explicit IoContext(const std::size_t num_threads, const IoOptions& opts = {}) : opts_(opts)
    {
        if (num_threads == 0)
        {
            throw std::runtime_error("io context started with 0 thread");
        }

        workers_.reserve(num_threads);

        // Leader (Worker 0)
        workers_.emplace_back(std::make_unique<IO>(0, stop_source_.get_token(), -1, opts_));
        const int leader_fd = workers_[0]->ring_fd();

        // Followers
        for (std::size_t i = 1; i < num_threads; ++i)
        {
            workers_.emplace_back(std::make_unique<IO>(i, stop_source_.get_token(), leader_fd, opts_));
        }
    }

    ~IoContext() = default;

    bool stop() { return stop_source_.request_stop(); }

    std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

    [[nodiscard]] IO& worker(const std::size_t idx) const { return *workers_[idx]; }
    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

    void join() noexcept
    {
        (void)stop();
        workers_.clear();
    }
};

// ============================================================================
// TransferTo — cross-thread execution transfer
// ============================================================================
struct TransferTo
{
    IO& target;

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept { target.post(h); }

    void await_resume() noexcept {}
};

// ============================================================================
// IoAwaiter Implementation
// This must be defined after IO is fully defined to avoid incomplete type errors.
// ============================================================================
template <typename SetupFunc, typename MapperFunc>
    requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
template <typename Promise>
std::coroutine_handle<> IoAwaiter<SetupFunc, MapperFunc>::await_suspend(std::coroutine_handle<Promise> h) noexcept
{
    io_uring_sqe* sqe = io_.get_sqe();
    if (sqe == nullptr)
    {
        ops_.res = -ENOSPC;
        return h;
    }

    this->ops_.h = h;
    setup_(sqe);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&ops_));

    return std::noop_coroutine();
}

}  // namespace URing
