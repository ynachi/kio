#pragma once
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

#include <liburing.h>

#include "buffer_pool.hpp"
#include "detail/queue.hpp"
#include "uring/core/awaiter.hpp"
#include "uring/core/task.hpp"
#include "uring/fd.hpp"
#include "uring/logger.hpp"

namespace kio
{
    //
    // Uring options
    //
    struct IoOptions
    {
        std::uint32_t entries = 16800;
        unsigned flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

        // Share one kernel worker pool (the leader's) across every ring via
        // IORING_SETUP_ATTACH_WQ. Keeping it true means N rings but one shared
        // backend, so the kernel balances submissions across a fixed pool of
        // worker threads. Setting it false gives every worker its own ring and
        // its own pool: true isolation, at the cost of N× the kernel threads.
        bool share_work_queues = true;

        // list of cpus, if empty, no pinning
        std::vector<int> worker_cpu_affinity{};

        /// Max cross-thread tasks (schedule()/TransferTo) moved off the MPSC queue
        /// per tick. Completion resumes per tick are capped separately by
        /// kMaxResumesPerTick. Values below 1 are treated as 1.
        std::size_t batch_max_size = 128;

        // How long teardown waits for in-flight operations to finish on their
        // own before escalating to IORING_ASYNC_CANCEL_ANY. Bounded by default:
        // an unbounded drain wedges forever on a single stuck read.
        std::uint32_t shutdown_grace_ms = 5000;

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
        friend class RegisteredBufferPool;
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

            void await_resume() const noexcept
            {
            }
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
         * @param pool_cfg Optional BufferPoolConfig to initialize a contiguous RegisteredBufferPool.
         *
         * @code
         * // 1. Standalone instance without fixed buffer pool
         * kio::IO io(0);
         *
         * // 2. With Registered Buffer Pool (e.g., 64 MiB total, 64 KiB slots)
         * kio::IO io_with_pool(0, nullptr, {}, kio::BufferPoolConfig{
         *     .slot_size = 65536,
         *     .slots = 1024
         * });
         * @endcode
         */
        explicit IO(size_t id, const IO* leader = nullptr, const IoOptions& opts = {},
                    std::optional<BufferPoolConfig> pool_cfg = std::nullopt);
        /// An IO can be moved only while inert: before run*()/sync_wait, with nothing
        /// scheduled. That allows object pools to be built by value. Moving an
        /// activated, running, or non-empty IO terminates the program; use a
        /// std::unique_ptr<IO> instead. Any BufferLease taken before a move still
        /// points at the old pool and is rejected by the fixed-buffer helpers.
        IO(IO&& other) noexcept;
        IO(const IO&) = delete;
        IO& operator=(const IO&) = delete;
        IO& operator=(IO&&) = delete;
        /// Tasks still queued at destruction (never resumed) are abandoned: their
        /// frames are leaked and a warning is logged. run_blocking() drains before
        /// it returns, so this only affects an IO destroyed without being run.
        ~IO();

        /// Run an event loop.
        /// Shutdown is coordinated externally, by the caller's provided stop token
        void run_blocking(std::stop_token st) noexcept;

        /// Run until done, no loop
        void run_once() noexcept
        {
            // Claim ring ownership before activate(), which submits SQEs.
            // See the owner_thread_ comment below.
            owner_thread_ = std::this_thread::get_id();
            pin_to_cpu();
            activate();
            tick();
        }

        /// Background task or post job to an io in another thread
        void schedule(Task<void> task)
        {
            // Admission gate. Once stop has been requested this IO accepts no
            // new work: teardown escalates to IORING_ASYNC_CANCEL_ANY, and a
            // task accepted during the drain could submit fresh I/O while
            // unwinding and hang again. Dropping here (rather than in post())
            // means the Task destructor owns the frame, as usual.
            if (stopping()) [[unlikely]]
            {
                KIO_LOG_DEBUG("schedule() refused: worker {} is stopping", id_);
                return;
            }

            auto h = task.release();
            auto* p = static_cast<detail::TaskPromiseBase*>(&h.promise());
            p->self_handle = h;
            post(p);
        }

        /// @brief True once shutdown has been requested for this IO.
        /// New operations are refused from this point on.
        [[nodiscard]] bool stopping() const noexcept
        {
            return stop_requested_.load(std::memory_order_seq_cst);
        }

        [[nodiscard]] static auto schedule_on(IO& target) noexcept { return TransferTo{target}; }

        void pin_to_cpu() const;

        [[nodiscard]] std::uint32_t id() const noexcept { return static_cast<std::uint32_t>(id_); }

        /// @brief Non-blocking attempt to acquire a buffer from the registered pool.
        [[nodiscard]] BufferLease try_acquire_buffer() noexcept
        {
            if (!buffer_pool_.has_value()) [[unlikely]]
                return {};
            return buffer_pool_->try_acquire();
        }

        /// @brief Asynchronously acquire a buffer with FIFO backpressure.
        [[nodiscard]] auto acquire_buffer() noexcept
        {
            struct Awaiter
            {
                IO& io;
                std::optional<RegisteredBufferPool::AcquireAwaiter> inner{};

                explicit Awaiter(IO& self) noexcept : io(self)
                {
                    if (io.buffer_pool_.has_value())
                        inner.emplace(*io.buffer_pool_);
                }

                bool await_ready() noexcept
                {
                    if (!inner.has_value())
                        return true;
                    return inner->await_ready();
                }

                void await_suspend(std::coroutine_handle<> h) noexcept
                {
                    assert(inner.has_value());
                    inner->await_suspend(h);
                }

                Result<BufferLease> await_resume() noexcept
                {
                    if (!inner.has_value())
                        return Error::fail_errc(std::errc::not_supported, "buffer pool not configured");
                    return inner->await_resume();
                }
            };

            return Awaiter{*this};
        }

        /// @brief Query the registered buffer pool, if configured.
        [[nodiscard]] const std::optional<RegisteredBufferPool>& buffer_pool() const noexcept
        {
            return buffer_pool_;
        }

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
                        return kio::Error::fail_errno(-res);
                    return Fd{res};
                });
        }

        [[nodiscard]] auto accept(Fd& server_fd, const int flags = kDefaultAcceptFlags)
        {
            return IoAwaiter(
                *this, [raw_fd = server_fd.fd, flags](io_uring_sqe* sqe)
                {
                    io_uring_prep_accept(sqe, raw_fd, nullptr, nullptr, flags);
                },
                [](const int32_t res) -> Result<Fd>
                {
                    if (res < 0)
                        return Error::fail_errno(-res);
                    return Fd{res};
                });
        }

        [[nodiscard]] auto read(Fd& fd, std::span<std::byte> buf, off_t offset = -1)
        {
            return IoAwaiter(
                *this, [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
                {
                    io_uring_prep_read(sqe, raw_fd, buf.data(), static_cast<unsigned>(buf.size()),
                                       static_cast<__u64>(offset));
                }, detail::ResumeInt{});
        }

        // Kernel-side write, for file I/O and any stream that cannot raise SIGPIPE.
        [[nodiscard]] auto write(Fd& fd, std::span<const std::byte> buf, off_t offset = -1)
        {
            return IoAwaiter(
                *this, [raw_fd = fd.fd, buf, offset](io_uring_sqe* sqe)
                {
                    io_uring_prep_write(sqe, raw_fd, buf.data(), static_cast<unsigned>(buf.size()),
                                        static_cast<__u64>(offset));
                }, detail::ResumeInt{});
        }

        // Socket send. io_uring_prep_write() leaves sqe->msg_flags at 0, so writing to
        // a peer that has already disconnected raises SIGPIPE and takes the whole
        // process down -- one dead client kills the server. prep_send() carries
        // msg_flags, so MSG_NOSIGNAL turns that into an EPIPE completion the
        // caller can handle.
        // No offset parameter: sockets are not written positionally.
        [[nodiscard]] auto send(Fd& fd, std::span<const std::byte> buf)
        {
            return IoAwaiter(
                *this, [raw_fd = fd.fd, buf](io_uring_sqe* sqe)
                {
                    io_uring_prep_send(sqe, raw_fd, buf.data(), buf.size(), MSG_NOSIGNAL);
                }, detail::ResumeInt{});
        }

        [[nodiscard]] auto read_fixed(Fd& fd, std::span<std::byte> buf, uint32_t buf_index, off_t offset = -1)
        {
            return IoAwaiter(
                *this, [raw_fd = fd.fd, buf, buf_index, offset](io_uring_sqe* sqe)
                {
                    io_uring_prep_read_fixed(sqe, raw_fd, buf.data(), static_cast<unsigned>(buf.size()),
                                             static_cast<__u64>(offset), static_cast<int>(buf_index));
                }, detail::ResumeInt{});
        }

        [[nodiscard]] auto write_fixed(Fd& fd, std::span<const std::byte> buf, uint32_t buf_index, off_t offset = -1)
        {
            return IoAwaiter(
                *this, [raw_fd = fd.fd, buf, buf_index, offset](io_uring_sqe* sqe)
                {
                    io_uring_prep_write_fixed(sqe, raw_fd, buf.data(), static_cast<unsigned>(buf.size()),
                                              static_cast<__u64>(offset), static_cast<int>(buf_index));
                },
                detail::ResumeInt{});
        }

        [[nodiscard]] auto writev(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
        {
            return IoAwaiter(
                *this, [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
                {
                    io_uring_prep_writev(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()),
                                         static_cast<__u64>(offset));
                },
                detail::ResumeInt{});
        }

        [[nodiscard]] auto open(std::filesystem::path path, const int flags, const mode_t mode = 0644)
        {
            return IoAwaiter(
                *this,
                [path, flags, mode](io_uring_sqe* sqe)
                {
                    io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode);
                },
                [](const int32_t res) -> Result<Fd>
                {
                    if (res < 0)
                    {
                        return Error::fail_errno(-res);
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
                    io_uring_prep_close(sqe, fd.Release()); // Release only once an SQE exists
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
                {
                    io_uring_prep_fsync(sqe, raw_fd, full_sync ? 0u : IORING_FSYNC_DATASYNC);
                }, detail::ResumeVoid{});
        }

        [[nodiscard]] auto fallocate(Fd& fd, const int mode, const off_t offset, const off_t len)
        {
            return IoAwaiter(
                *this, [raw_fd = fd.fd, mode, offset, len](io_uring_sqe* sqe)
                {
                    io_uring_prep_fallocate(sqe, raw_fd, mode, static_cast<__u64>(offset), static_cast<__u64>(len));
                }, detail::ResumeVoid{});
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
                *this, [raw_fd = fd.fd, poll_mask](io_uring_sqe* sqe)
                {
                    io_uring_prep_poll_add(sqe, raw_fd, poll_mask);
                },
                [](const int32_t res) -> Result<unsigned>
                {
                    if (res < 0)
                        return Error::fail_errno(-res);
                    return static_cast<unsigned>(res);
                });
        }

        /// Relative duration to a kernel timespec. Negative durations clamp to zero
        /// (the kernel rejects a negative tv_nsec) and the nanosecond part is taken
        /// from the sub-second remainder, so huge durations cannot overflow.
        template <typename Rep, typename Period>
        [[nodiscard]] static __kernel_timespec to_timespec(const std::chrono::duration<Rep, Period> dur) noexcept
        {
            using namespace std::chrono;
            if (dur <= dur.zero())
                return {};
            const auto secs = duration_cast<seconds>(dur);
            return __kernel_timespec{
                .tv_sec = static_cast<__kernel_time64_t>(secs.count()),
                .tv_nsec = static_cast<long long>(duration_cast<nanoseconds>(dur - secs).count())};
        }

        template <typename Rep, typename Period>
        [[nodiscard]] auto timeout(const std::chrono::duration<Rep, Period> dur)
        {
            return IoAwaiter(
                *this,
                [ts = to_timespec(dur)](io_uring_sqe* sqe) mutable
                {
                    io_uring_prep_timeout(sqe, &ts, 0, 0);
                },
                [](const int32_t res) -> Result<void>
                {
                    if (res == -ETIME || res == 0)
                        return {};
                    return kio::Error::fail_errno(-res);
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
                {
                    io_uring_prep_connect(sqe, raw_fd, addr.Get(), addr.addrlen);
                }, detail::ResumeVoid{});
        }

        [[nodiscard]] auto readv(Fd& fd, std::span<const iovec> iovecs, off_t offset = -1)
        {
            return IoAwaiter(
                *this, [raw_fd = fd.fd, iovecs, offset](io_uring_sqe* sqe)
                {
                    io_uring_prep_readv(sqe, raw_fd, iovecs.data(), static_cast<unsigned>(iovecs.size()),
                                        static_cast<__u64>(offset));
                },
                detail::ResumeInt{});
        }

        [[nodiscard]] auto rename(std::filesystem::path from, std::filesystem::path to)
        {
            return IoAwaiter(
                *this, [from = std::move(from), to = std::move(to)](io_uring_sqe* sqe)
                {
                    io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0);
                }, detail::ResumeVoid{});
        }

        // ============================================================================
        // io.hpp extensions: Fixed-buffer I/O helpers
        // ============================================================================

        /// @brief Async read using a pre-registered fixed buffer
        [[nodiscard]] auto read_fixed(Fd& fd,
                                      BufferLease& buf,
                                      const size_t len,
                                      const size_t buffer_offset = 0,
                                      off_t file_offset = -1)
        {
            const size_t safe_len = (buffer_offset < buf.size())
                                        ? std::min(len, buf.size() - buffer_offset)
                                        : 0;

            auto* ptr = buf.data() + buffer_offset;
            // An empty lease, or one from another IO's pool, has no valid index in
            // this ring's table. Submit against fd -1 so it completes with -EBADF
            // instead of dereferencing a null pool or using a foreign index.
            const bool usable = owns(buf);
            const uint16_t buf_index = usable ? buf.pool()->registration_index(ptr) : 0;

            return IoAwaiter(
                *this,
                [raw_fd = usable ? fd.fd : -1, ptr, safe_len, buf_index, file_offset](io_uring_sqe* sqe)
                {
                    io_uring_prep_read_fixed(sqe, raw_fd, ptr, static_cast<unsigned>(safe_len),
                                             static_cast<__u64>(file_offset), buf_index);
                },
                detail::ResumeInt{});
        }

        /// @brief read to fill the buffer
        [[nodiscard]] auto read_fixed(Fd& fd, BufferLease& buf, const off_t file_offset = -1)
        {
            return read_fixed(fd, buf, buf.size(), /*buffer_offset=*/0, file_offset);
        }

        /// @brief Async write using a pre-registered fixed buffer
        [[nodiscard]] auto write_fixed(Fd& fd,
                                       const BufferLease& buf,
                                       const size_t len,
                                       const size_t buffer_offset = 0,
                                       off_t file_offset = -1)
        {
            const size_t safe_len = (buffer_offset < buf.size())
                                        ? std::min(len, buf.size() - buffer_offset)
                                        : 0;

            const auto* ptr = buf.data() + buffer_offset;
            const bool usable = owns(buf);
            const uint16_t buf_index = usable ? buf.pool()->registration_index(ptr) : 0;

            return IoAwaiter(
                *this,
                [raw_fd = usable ? fd.fd : -1, ptr, safe_len, buf_index, file_offset](io_uring_sqe* sqe)
                {
                    io_uring_prep_write_fixed(sqe, raw_fd, ptr, static_cast<unsigned>(safe_len),
                                              static_cast<__u64>(file_offset), static_cast<int>(buf_index));
                },
                detail::ResumeInt{});
        }

        /// @brief write the entire buffer
        [[nodiscard]] auto write_fixed(Fd& fd, const BufferLease& buf, const off_t file_offset = -1)
        {
            return write_fixed(fd, buf, buf.size(), /*buffer_offset=*/0, file_offset);
        }

    private:
        /// True if the lease is live and belongs to this IO's registered pool.
        [[nodiscard]] bool owns(const BufferLease& lease) const noexcept
        {
            return lease && buffer_pool_.has_value() && lease.pool() == &*buffer_pool_;
        }

        static constexpr uint64_t kWakeupSentinel = 0xDEAD'C0DE'DEAD'C0DEULL;
        static constexpr uint64_t kCancelSentinel = 0xCACE'1ED0'CACE'1ED0ULL;
        static constexpr size_t kMaxResumesPerTick = 128;

        io_uring ring_{};
        int wake_fd_{-1};
        uint64_t wake_value_{0};
        bool is_activated_{false};
        bool is_running_{false};
        // One-shot stop latch, set by the stop callback before wake(). The
        // pre-sleep recheck loads it so a stop landing between the loop's
        // stop_requested() test and the park cannot be lost.
        //
        // seq_cst on both sides is what makes that safe: the store and the load
        // share one total order, so either the worker's load precedes the stop
        // (and it parks only after wake() has already written the eventfd) or
        // the load observes the flag and never parks. A weaker pairing here
        // would reopen the lost-wakeup window that Option A closed.
        std::atomic<bool> stop_requested_{false};
        IoOptions opts_;
        /// This is not a typical id, it is use for CPU pining too
        size_t id_;
        // The ring is created in SINGLE_ISSUER mode, so SQEs must only ever be
        // produced by the thread that runs this IO. After co_await
        // io.schedule_on(other), the awaiting chain continues on the *other*
        // thread, so a following co_await io.read(...) would submit to this ring
        // from the wrong thread. get_sqe() asserts on this.
        //
        // This is claimed by the thread that *drives* the ring -- set at the top
        // of run_blocking()/run_once() -- because IoContext constructs every IO
        // on the caller's thread and only afterwards hands each to a jthread.
        // Cross-thread schedule()/post() stay safe: they use a lock-free queue
        // plus the eventfd, never get_sqe().
        std::thread::id owner_thread_{};
        std::vector<std::coroutine_handle<>> local_tasks_{};
        std::vector<std::coroutine_handle<>> current_batch{};
        detail::CoroQueue queue_{};

        std::optional<RegisteredBufferPool> buffer_pool_{std::nullopt};
        std::vector<iovec> registered_iovecs_{};
        // CQEs consumed off the ring so far. Every SQE the user writes produces
        // exactly one CQE, so (ktail - completed_count_) is the number of
        // operations the kernel still owns. SQEs not yet flushed are not in ktail;
        // outstanding_ops() adds them back via unflushed_sqes().
        // Derived from ring state rather than a per-op counter, so quiescence
        // costs one subtraction instead of a store on the I/O path.
        std::uint64_t completed_count_{0};
        // Wake reads currently in the ring. They are internal plumbing, not user
        // work, so they must not count as outstanding or every shutdown would
        // wait out the full grace period for a read that is supposed to hang.
        std::uint32_t armed_wake_reads_{0};

        /// Creates the Ring in an uninitialized way
        void init(int wq_fd = -1);
        /// Activate a disabled ring, MUST be called after init()
        /// Enable a ring created with IORING_SETUP_R_DISABLED. noexcept: callers
        /// include run_blocking()/run_once(), where a throw would terminate the
        /// worker. Failure is logged FATAL (aborts): the ring is unusable.
        void activate() noexcept;
        void tick(__kernel_timespec* park_timeout = nullptr) noexcept;
        /// @param park_timeout If non-null, the blocking park is bounded by
        ///        this timeout instead of waiting indefinitely. Teardown uses a
        ///        bound so a stuck operation cannot wedge the worker forever.
        void submit_or_wait_for(__kernel_timespec* park_timeout = nullptr);
        /// Graceful drain bounded by opts_.shutdown_grace_ms, escalating to
        /// IORING_ASYNC_CANCEL_ANY and then draining the cancellations.
        void drain_until_quiescent() noexcept;
        void submit_cancel_all() noexcept;
        /// SQEs prepared by get_sqe() but not yet flushed to the kernel tail.
        [[nodiscard]] std::uint32_t unflushed_sqes() const noexcept
        {
            return ring_.sq.sqe_tail - ring_.sq.sqe_head;
        }
        /// Operations in flight or queued, excluding internal wake reads: those
        /// flushed to the kernel and not yet completed, plus those still sitting
        /// unflushed in the SQ. Owner-thread only; no atomics.
        ///
        /// ktail is a free-running 32-bit counter that wraps, so the difference is
        /// taken in 32 bits; widening first would underflow after 2^32 operations.
        [[nodiscard]] std::uint64_t outstanding_ops() const noexcept
        {
            const auto submitted =
                static_cast<std::uint32_t>(*ring_.sq.ktail) - static_cast<std::uint32_t>(completed_count_);
            return std::uint64_t{submitted} + unflushed_sqes() - armed_wake_reads_;
        }
        /// Anything left for teardown to wait on or run.
        [[nodiscard]] bool has_pending_work() const noexcept;
        void arm_wake_read() noexcept;
        void wake() const noexcept;
        io_uring_sqe* get_sqe() noexcept;

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
        // Admission gate: no new operations once stop has been requested.
        // ECANCELED is the same result a cancelled in-flight op receives, so a
        // coroutine unwinding through teardown sees one consistent error.
        if (io_.stopping()) [[unlikely]]
        {
            ops_.res = -ECANCELED;
            return h;
        }

        io_uring_sqe* sqe = io_.get_sqe();
        if (sqe == nullptr)
        {
            KIO_LOG_WARN("SQ ring full; returning EAGAIN to apply backpressure");
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
    namespace detail
    {
    /// Body of sync_wait, as a named coroutine rather than a lambda. A capturing
    /// lambda would be a temporary destroyed at the end of the call expression,
    /// while the coroutine it builds has not run yet (initial_suspend is
    /// suspend_always) and still holds the dead closure's `this` -- ASan reported
    /// stack-use-after-scope when it was finally resumed.
    template <typename T>
    Task<void> sync_wait_body(Task<T> t, std::optional<Result<T>>& out, bool& done)
    {
        out = co_await std::move(t);
        done = true;
        co_return {};
    }
    } // namespace detail

    /// Testing utility, block a coroutine until it is done
    template <typename T>
    Result<T> sync_wait(IO& io, Task<T>&& task)
    {
        bool done{false};
        std::optional<Result<T>> result;

        io.schedule(detail::sync_wait_body<T>(std::move(task), result, done));

        // Claim ring ownership before activate(), which submits SQEs
        // (see IO::owner_thread_).
        io.owner_thread_ = std::this_thread::get_id();
        io.pin_to_cpu();
        io.activate();

        while (!done)
        {
            io.tick();
        }

        return std::move(*result);
    }
} // namespace kio
