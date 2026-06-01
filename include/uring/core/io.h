#pragma once
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

#include <liburing.h>

#include <boost/context/detail/fcontext.hpp>

#include "buffer_pool.hpp"
#include "detail/queue.hpp"
#include "detail/fiber_queue.hpp"
#include "uring/core/awaiter.hpp"
#include "uring/core/task.hpp"
#include "uring/fd.hpp"
#include "uring/logger.hpp"
#include "uring/core/fiber.hpp"

#include <list>

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

    // list of cpus, if empty, no pinning
    std::vector<int> worker_cpu_affinity{};

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
    struct TransferTo;

    friend class IoContext;
    friend struct TransferTo;
    template <typename SetupFunc, typename MapperFunc>
        requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
    friend class IoAwaiter;
    friend class FiberIO;
    friend class FixedBufferPool;
    template <typename T>
    friend Result<T> sync_wait(IO&, Task<T>&&);

    struct TransferTo
    {
        IO& target;

        // Optimization: If we are already on the target thread, don't suspend at all.
        bool await_ready() const noexcept { return false; }

        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> h) noexcept
        {
            // Get the base promise pointer
            auto* p = static_cast<detail::TaskPromiseBase*>(&h.promise());

            // Store the erased handle so the target thread can resume it
            p->self_handle = h;

            // Post it to the intrusive queue
            target.post(p);
        }

        void await_resume() const noexcept {}
    };

public:
    static constexpr unsigned kUringDefaultFlag =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    static constexpr int kDefaultAcceptFlags = SOCK_NONBLOCK | SOCK_CLOEXEC;

    /**
     * @brief Construct a new IO worker.
     *
     * @param id The unique identifier for this worker. Also used as an index for CPU pinning
     *           via IoOptions::worker_cpu_affinity.
     * @param leader Optional pointer to a "leader" IO instance. If provided, this worker
     *               will share the same kernel workqueue (IORING_SETUP_ATTACH_WQ).
     * @param opts Configuration options for the io_uring ring.
     * @param pool_configs Optional list of {size, count} bucket configurations to initialize
     *                     a FixedBufferPool for zero-copy I/O.
     *
     * @code
     * // 1. Standalone instance
     * URing::IO io(0);
     *
     * // 2. Scaling with leader-follower pattern
     * URing::IO leader(0);
     * URing::IO follower(1, &leader);
     *
     * // 3. With Fixed Buffer Pool for zero-copy
     * URing::IO io_with_pool(0, nullptr, {}, {
     *     { .size = 4096, .count = 1024 }, // 1024 buffers of 4KB
     *     { .size = 65536, .count = 128 }  // 128 buffers of 64KB
     * });
     * @endcode
     */
    explicit IO(size_t id, const IO* leader = nullptr, const IoOptions& opts = {},
                std::initializer_list<BucketConfig> pool_configs =
                    {});  /// IO object can be moved but with some limitations. Before it start doing some actual io
                          /// (before run*()),
    /// its is safe to move it. Because, in this state, it's an inert object. So it gives you more flexibilities
    /// on the object and object pools construction. But, it SHOULD not be moved after it started doing IO.
    /// If you need to do it for some reason, use a std::unique_ptr<IO>. Moving the direct object while IO is active
    /// will terminate the program.
    IO(IO&& other) noexcept;
    IO(const IO&) = delete;
    IO& operator=(const IO&) = delete;
    IO& operator=(IO&&) = delete;
    ~IO();

    /// Run an event loop.
    /// Shutdown is coordinated externally, by the caller's provided stop token
    void run_blocking(std::stop_token st) noexcept;

    /// Run until done, no loop
    void run_once() noexcept
    {
        pin_to_cpu();
        activate();
        tick();
    }

    /// Background task or post job to an io in another thread
    void schedule(Task<void> task)
    {
        auto h = task.release();
        auto* p = static_cast<detail::TaskPromiseBase*>(&h.promise());
        p->self_handle = h;
        post(p);
    }

    [[nodiscard]] auto schedule_on(IO& target) noexcept { return TransferTo{target}; }

    /// Launch a new stackful fiber on this IO.  fn is called as fn(fio) where
    /// fio provides a synchronous-looking I/O API that suspends only the fiber
    /// (never the OS thread).  IO takes ownership; the fiber is destroyed when
    /// fn returns.  stack_size is the fiber stack in bytes (e.g. 64 * 1024).
    ///
    /// MUST be called from the thread that owns this IO.  For cross-thread
    /// fiber spawning use schedule_fiber() instead.
    template <std::invocable<FiberIO&> Fn>
    void spawn_fiber(Fn&& fn, const size_t stack_size = kDefaultFiberStack)
    {
        auto ctx    = std::make_unique<FiberContext>(stack_size);
        ctx->io     = this;
        ctx->fn     = std::forward<Fn>(fn);
        // Stack grows downward.  The usable region starts just above the guard
        // page; its top (highest address) is the stack pointer make_fcontext needs.
        auto* stack_top = static_cast<char*>(ctx->stack_mem) + kFiberGuardPageSize + stack_size;
        ctx->ctx    = boost::context::detail::make_fcontext(
            stack_top, stack_size, detail::fiber_entry);

        FiberContext* raw = ctx.get();
        link_fiber(raw);
        try
        {
            ready_fibers_.push_back(raw);
        }
        catch (...)
        {
            detach_fiber(raw);
            throw;
        }
        ctx.release();
    }

    /// Cross-thread safe fiber spawn.  Constructs the fiber fully on the caller
    /// and enqueues the raw FiberContext* into the dedicated fiber MPSC queue.
    /// tick() drains it, takes ownership, and queues the fiber for its first
    /// resume — so the fiber is born on the target thread and never migrates.
    ///
    /// Safe to call from any thread.  fn is moved once into FiberContext::fn.
    template <std::invocable<FiberIO&> Fn>
    void schedule_fiber(Fn&& fn, const size_t stack_size = kDefaultFiberStack)
    {
        auto ctx    = std::make_unique<FiberContext>(stack_size);
        ctx->io     = this;
        ctx->fn     = std::forward<Fn>(fn);
        auto* stack_top = static_cast<char*>(ctx->stack_mem) + kFiberGuardPageSize + stack_size;
        ctx->ctx    = boost::context::detail::make_fcontext(
            stack_top, stack_size, detail::fiber_entry);
        fiber_queue_.enqueue(ctx.release());
        wake();
    }

    void pin_to_cpu() const;

    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }

    Result<void> register_buffers(FixedBufferPool& pool) noexcept
    {
        if (pool.is_registered())
        {
            return std::unexpected(make_error_code(PoolError::AlreadyRegistered));
        }

        if (const int ret = io_uring_register_buffers(&ring_, pool.iovecs_ptr(), pool.total_capacity()); ret < 0)
        {
            return std::unexpected(error_from_errno(-ret));
        }

        pool.set_registered();

        return {};
    }

    Result<void> unregister_buffers() noexcept
    {
        if (const int ret = io_uring_unregister_buffers(&ring_); ret < 0)
        {
            return std::unexpected(error_from_errno(-ret));
        }
        return {};
    }

    [[nodiscard]] Result<FixedBuffer> take_fixed_buffer(const size_t size) noexcept { return buffer_pool_.take(size); }

    //
    // IO Methods
    //
    [[nodiscard]] auto accept(Fd& server_fd, SocketAddress& client_addr, const int flags = kDefaultAcceptFlags)
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
                    return std::unexpected(make_error_code(res));
                return Fd{res};
            });
    }

    [[nodiscard]] auto accept(Fd& server_fd, const int flags = kDefaultAcceptFlags)
    {
        return IoAwaiter(
            *this, [raw_fd = server_fd.fd, flags](io_uring_sqe* sqe)
            { io_uring_prep_accept(sqe, raw_fd, nullptr, nullptr, flags); },
            [](const int32_t res) -> Result<Fd>
            {
                if (res < 0)
                    return std::unexpected(make_error_code(res));
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

    [[nodiscard]] auto read_fixed(Fd& fd, std::span<std::byte> buf, uint32_t buf_index, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, buf, buf_index, offset](io_uring_sqe* sqe)
            { io_uring_prep_read_fixed(sqe, raw_fd, buf.data(), buf.size(), offset, buf_index); }, detail::ResumeInt{});
    }

    [[nodiscard]] auto write_fixed(Fd& fd, std::span<const std::byte> buf, uint32_t buf_index, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, buf, buf_index, offset](io_uring_sqe* sqe)
            { io_uring_prep_write_fixed(sqe, raw_fd, buf.data(), buf.size(), offset, buf_index); },
            detail::ResumeInt{});
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
                {
                    return std::unexpected(make_error_code(res));
                }
                return Fd{res};
            });
    }

    [[nodiscard]] auto close(Fd&& fd)
    {
        return IoAwaiter(
            *this,
            [fd = std::move(fd)](io_uring_sqe* sqe) mutable
            {
                io_uring_prep_close(sqe, fd.Release());  // Release only once an SQE exists
            },
            detail::ResumeVoid{});
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
            [](const int32_t res) -> Result<unsigned>
            {
                if (res < 0)
                    return std::unexpected(make_error_code(res));
                return static_cast<unsigned>(res);
            });
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
                return std::unexpected(make_error_code(res));
            });
    }

    template <typename Rep, typename Period>
    [[nodiscard]] auto sleep(const std::chrono::duration<Rep, Period> dur)
    {
        return timeout(dur);
    }

    /// addr MUST outlive the connect method
    [[nodiscard]] auto connect(Fd& fd, const SocketAddress& addr)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, &addr](io_uring_sqe* sqe) mutable
            { io_uring_prep_connect(sqe, raw_fd, addr.Get(), addr.addrlen); }, detail::ResumeVoid{});
    }

    [[nodiscard]] auto readv(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
            { io_uring_prep_readv(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()), offset); },
            detail::ResumeInt{});
    }

    [[nodiscard]] auto rename(std::filesystem::path from, std::filesystem::path to)
    {
        return IoAwaiter(
            *this, [from = std::move(from), to = std::move(to)](io_uring_sqe* sqe)
            { io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0); }, detail::ResumeVoid{});
    }

    // ============================================================================
    // io.hpp extensions: Fixed-buffer I/O helpers
    // ============================================================================

    /// @brief Async read using a pre-registered fixed buffer
    [[nodiscard]] auto read_fixed(Fd& fd, FixedBuffer& buf, const size_t len, off_t offset = -1)
    {
        const size_t safe_len = std::min(len, buf.size());

        return IoAwaiter(
            *this, [raw_fd = fd.fd, ptr = buf.ptr(), safe_len, index = buf.index(), offset](io_uring_sqe* sqe)
            { io_uring_prep_read_fixed(sqe, raw_fd, ptr, safe_len, offset, index); }, detail::ResumeInt{});
    }

    /// @brief read to fill the buffer
    [[nodiscard]] auto read_fixed(Fd& fd, FixedBuffer& buf, const off_t offset = -1)
    {
        return read_fixed(fd, buf, buf.size(), offset);
    }

    /// @brief Async write using a pre-registered fixed buffer
    [[nodiscard]] auto write_fixed(Fd& fd, const FixedBuffer& buf, size_t len, off_t offset = -1)
    {
        const size_t safe_len = std::min(len, buf.size());

        return IoAwaiter(
            *this, [raw_fd = fd.fd, ptr = buf.ptr(), safe_len, index = buf.index(), offset](io_uring_sqe* sqe)
            { io_uring_prep_write_fixed(sqe, raw_fd, ptr, safe_len, offset, index); }, detail::ResumeInt{});
    }

    /// @brief write the entire buffer
    [[nodiscard]] auto write_fixed(Fd& fd, const FixedBuffer& buf, const off_t offset = -1)
    {
        return write_fixed(fd, buf, buf.size(), offset);
    }

private:
    static constexpr uint64_t kWakeupSentinel = 0xDEAD'C0DE'DEAD'C0DEULL;
    static constexpr uint64_t kFiberCancelSentinel = 0xDEAD'C0DE'DEAD'C0CCULL;
    static constexpr size_t kMaxResumesPerTick = 128;

    io_uring ring_{};
    int wake_fd_{-1};
    uint64_t wake_value_{0};
    bool is_activated_{false};
    bool is_running_{false};
    // cross-thread needs to check this, this is why it is an atomic
    alignas(64) std::atomic<bool> is_sleeping_{false};
    IoOptions opts_;
    /// This is not a typical id, it is use for CPU pining too
    size_t id_;
    std::vector<std::coroutine_handle<>> local_tasks_{};
    std::vector<std::coroutine_handle<>> current_batch{};
    detail::CoroQueue queue_{};

    FixedBufferPool buffer_pool_{};

    // ── Fiber support ────────────────────────────────────────────────────────
    // With Boost.Context, there is no single scheduler_ctx_ member.  Each
    // jump_fcontext call captures the caller's state implicitly in transfer_t,
    // so tick() and each fiber exchange context handles on every switch.
    detail::FiberQueue                        fiber_queue_{};
    std::vector<FiberContext*>                ready_fibers_{};
    FiberContext*                             owned_head_{nullptr};
    FiberContext*                             owned_tail_{nullptr};
    size_t                                    owned_count_{0};
    bool                                      canceling_fibers_{false};

    /// Creates the Ring in an uninitialized way
    void init(int wq_fd = -1);
    /// Activate a disabled ring, MUST be called after init()
    void activate();
    void tick() noexcept;
    void submit_or_wait_for();
    void arm_wake_read() noexcept;
    void wake() const noexcept;
    io_uring_sqe* get_sqe() noexcept;
    void link_fiber(FiberContext* ctx) noexcept;
    void detach_fiber(FiberContext* ctx) noexcept;
    void unlink_fiber(FiberContext* ctx) noexcept;
    void cancel_all_fibers() noexcept;

    int ring_fd() const noexcept { return ring_.ring_fd; }

    void post(detail::TaskPromiseBase* task)
    {
        queue_.enqueue(task);
        wake();
    }

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
        ALOG_WARN("SQ ring full; returning EAGAIN to apply backpressure");
        ops_.res = -EAGAIN;
        return h;
    }

    this->ops_.h = h;
    setup_(sqe);
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&ops_));

    return std::noop_coroutine();
}

//
// sync_wait testing util
//
/// Testing utility, block a coroutine until it is done
template <typename T>
Result<T> sync_wait(IO& io, Task<T>&& task)
{
    bool done{false};
    std::optional<Result<T>> result;

    io.schedule(
        [&](Task<T> t) -> Task<void>
        {
            result = co_await std::move(t);
            done = true;
            co_return {};
        }(std::move(task)));

    io.pin_to_cpu();
    io.activate();

    while (!done)
    {
        io.tick();
    }

    return std::move(*result);
}

}  // namespace URing
