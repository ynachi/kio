#pragma once

// ============================================================================
// kio::IO -- one worker: one io_uring ring, one MPSC queue, one thread
// ============================================================================
//
// THE SHAPE OF THE THING
// ----------------------
//
//   IoContext builds N IO objects on the CALLER's thread, then hands each one
//   to its own jthread running run_blocking(). Nothing is shared between
//   workers except (optionally) the kernel workqueue. Each IO owns:
//
//     - an io_uring ring          (kernel-side async context)
//     - a detail::CoroQueue      (intrusive MPSC inbound hand-off)
//     - local_tasks_ / current_batch  (owner-thread resume staging)
//
//   No fds, no wake channel: another thread's only way in is queue_.enqueue(),
//   and the worker picks it up on its next heartbeat tick. See I3.
//
//   Producers --> [ MPSC queue ] --> worker thread --> [ ring ] --> kernel
//                      |                                    |
//                      +--------- heartbeat park ----------+
//                        (bounded deadline, see I3)
//
// TWO INVARIANTS THAT HOLD THE WHOLE DESIGN TOGETHER
// ---------------------------------------------------
//
// I1. SINGLE_ISSUER -- one thread per IO produces SQEs.
//     The ring is created with IORING_SETUP_SINGLE_ISSUER, so the kernel
//     assumes (and may optimise for) a single thread calling into the ring.
//     IO::get_sqe() asserts it on every SQE.
//
//     The consequence bites hard: `co_await io.read(...)` runs wherever the
//     coroutine currently is, so after `co_await io.schedule_on(other)` the
//     continuation is on `other` and a following io.read() would submit from
//     the wrong thread. The fix is not a runtime check but a discipline: once
//     you transfer, you transfer for good.
//
//     Note that post()/schedule() from ANY thread are still perfectly legal.
//     They only push onto the lock-free queue, and never call get_sqe().
//
//     owner_thread_ is claimed by the thread that DRIVES the ring (set at the
//     top of run_blocking()/run_once()/sync_wait), never in the constructor --
//     because IoContext constructs every IO on the caller's thread, before any
//     jthread exists. Capturing in the ctor named the wrong thread.
//
// I2. THE FRAME IS THE OPERATION TABLE.
//     IoOps lives inside the coroutine frame that awaits the operation, and
//     user_data == &ops_. There is therefore no in-flight registry, no
//     cancellation map and no per-operation refcount. The full argument and its
//     limits are in core/awaiter.hpp.
//
// I3. THE PARK IS BOUNDED, AND THAT IS THE WHOLE TRICK
// --------------------------------------------------
//
// A worker with nothing to do sleeps inside io_uring_submit_and_wait_timeout()
// with a deadline of opts_.heartbeat_interval_us. Nothing has to be able to
// wake it, because it always comes back on its own:
//
//     producer:  queue_.enqueue(task)
//     worker:    ... parks at most heartbeat_interval_us ... drains, resumes
//
// A post that races the park is not a LOST wakeup, it is at worst one tick of
// latency. That distinction is the entire reason this loop is small. It is what
// lets post() be a bare queue push with no syscall, no eventfd, and no handshake
// with the worker.
//
// The alternative -- parking forever and interrupting -- was tried here first
// and it cost all of the following, none of which the deadline makes necessary:
//
//   * an eventfd with a read armed in the ring at all times
//   * a sentinel user_data, and a three-way branch in the CQ walk
//   * an "at most one wake read in flight" induction, to prove two reads could
//     never race on the same destination buffer
//   * may_park(), which then forced TWO full rechecks of every work source
//     before sleeping, to close the window the handshake opened
//   * a seq_cst store/load pairing between the stop callback and the park
//   * a third term in outstanding_ops(), whose omission bug made every shutdown
//     sit out the full grace period (11ms -> 10s)
//
// All six existed to answer one question -- "what if a post arrives while the
// worker is asleep?" -- which a deadline answers for free.
//
// IDLE COST. An idle worker wakes every opts_.max_idle_wait_us instead of
// sleeping until the next post. That is the price, and it is the only one.
// tick() backs off exponentially the longer it does nothing, so a busy worker
// parks rarely and an idle one settles on the longest interval.
//
// THE LOOP
// --------
//
//   run_blocking:
//     claim owner_thread_ -> pin -> activate
//     install stop_callback { stop_requested_ = true; }
//     while (!stop.stop_requested()) tick();
//     -> close buffer pool -> drain_until_quiescent()
//
//   tick():
//     1. drain up to batch_max_size cross-thread tasks into local_tasks_
//     2. submit + park for at most heartbeat_interval_us   <-- the only sleep
//     3. harvest up to batch_max_size CQEs, advancing as we go, queueing the
//        coroutine handle each one belongs to
//     4. resume everything queued, then restore the heartbeat interval
//
//   Steps 2 and 4 bracket one heartbeat tick. The CQ is harvested before
//   anything is resumed, so no user code ever runs with the ring's head
//   un-advanced, and the harvest is capped: CQEs beyond the cap stay in the
//   ring and are picked up next tick rather than being carried in a side
//   vector.
//
// TEARDOWN
// --------
//
//   drain_until_quiescent() is a two-phase bounded drain:
//
//     phase 1  tick(bounded) until has_pending_work() is false or the grace
//              deadline passes. Operations finish on their own if they can.
//     phase 2  if outstanding_ops() > 0, submit IORING_ASYNC_CANCEL_ANY and
//              keep ticking until quiescent. Cancelled ops complete with
//              -ECANCELED, which resumes their coroutines so they unwind
//              through the ordinary error path.
//
//   The admission gate is the other half of this. Once stop_requested_ is set,
//   schedule() refuses new tasks and IoAwaiter::await_suspend refuses new
//   operations with -ECANCELED. Without the gate, a task accepted mid-drain
//   could submit fresh I/O while teardown was unwinding, and the drain would
//   never reach quiescence.
//
// ============================================================================

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
#include "kio/core/awaiter.hpp"
#include "kio/core/task.hpp"
#include "kio/fd.hpp"
#include "kio/logger.hpp"

namespace kio
{
    //
    // Uring options
    //
    // INVARIANT: read once, in init(), and thereafter treated as immutable.
    // opts_ is a plain (non-atomic) member because nothing mutates it after
    // construction -- including the CPU-pinning decision, which is applied by
    // pin_to_cpu() from a copy that already exists by then.
    //
    // INVARIANT: worker_cpu_affinity is indexed by id_, so len(affinity) must be
    // >= the highest id in use. Entries beyond that are simply never applied
    // (pin_to_cpu() returns early) rather than being an error, which keeps a
    // short affinity list usable with fewer workers.
    struct IoOptions
    {
        // SQ size. Operations in flight do not occupy SQ slots; it only has to hold
        // what is prepared between two submits (get_sqe() flushes when it fills),
        // so a small ring is better: producer and consumer cycle through the whole
        // ring, and a multi-MiB ring means every SQE/CQE touches a cold cache line.
        // The CQ defaults to twice this and overflow is handled by the kernel.
        std::uint32_t entries = 4096;
        unsigned flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

        // Share one kernel worker pool (the leader's) across every ring via
        // IORING_SETUP_ATTACH_WQ. Keeping it true means N rings but one shared
        // backend, so the kernel balances submissions across a fixed pool of
        // worker threads. Setting it false gives every worker its own ring and
        // its own pool: true isolation, at the cost of N× the kernel threads.
        bool share_work_queues = true;

        // list of cpus, if empty, no pinning
        std::vector<int> worker_cpu_affinity{};

        /// Max work moved per tick. Applies to BOTH cross-thread tasks drained
        /// from the MPSC queue and CQEs harvested off the ring; whatever
        /// overflows is left for the next tick rather than carried in a side
        /// vector. Bounds how long one tick can run without re-checking the stop
        /// flag. Values below 1 are treated as 1.
        std::size_t batch_max_size = 128;

        /// Park deadline while the worker has work flowing. This is the upper
        /// bound on how long a cross-thread post() waits to be noticed, so it
        /// is a latency knob first and an idle-CPU knob second. It only applies
        /// when the tick actually had nothing to do -- a tick that found CQEs or
        /// tasks never parks at all.
        std::uint32_t heartbeat_interval_us = 50;

        /// Ceiling the idle backoff doubles towards. An idle worker parks for
        /// this long every time; a busy one never reaches it. Raise it to trade
        /// idle CPU for cross-thread wake latency, lower it for the reverse.
        std::uint32_t max_idle_wait_us = 2000;

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
    //
    // Corrections/additions to the above, in the light of how this actually
    // behaves -- the original bullets were written before the invariants in
    // the file header were pinned down:
    //
    //   - "TransferTo is the ONLY cross-thread mechanism" is true only for
    //     moving a COROUTINE. Moving an IO object (the move ctor) is inert
    //     only, and post()/schedule() are the runtime half of the same idea.
    //   - "MPSC queue holds raw coroutine_handle<>" is a half-truth: the queue
    //     is typed to TaskPromiseBase*, and the handle is stashed alongside it
    //     in promise::self_handle because the queue needs no type erasure.
    //   - h.resume() is safe not merely because handles are enqueued while
    //     suspended, but because there is exactly ONE resume site per handle
    //     (the owner thread) and exactly one enqueue (the queue), so no handle
    //     can be resumed twice or concurrently.
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

        // The awaitable behind IO::schedule_on(io): "run the rest of this
        // coroutine on THAT worker instead".
        //
        //   co_await io.schedule_on(other);
        //   co_await io2.read(...);   // now runs on `other`
        //
        // Combined with invariant I1, this is a one-way door. After the
        // transfer, the coroutine lives on `other`'s thread, and any further
        // submission through the ORIGINAL io would violate SINGLE_ISSUER.
        // get_sqe() asserts rather than silently corrupting the ring.
        struct TransferTo
        {
            IO& target;

            // Always false. An earlier version intended to short-circuit when
            // the caller already happened to be on the target's thread, but
            // that test costs a thread-id comparison on every transfer to buy
            // a case that is rare by construction (a worker transfers to
            // another worker, not to itself). The comment above used to
            // describe that abandoned optimisation; it does not describe this
            // code. Suspending unconditionally is also the only correct
            // option: await_suspend() must publish self_handle and enqueue
            // before the target can possibly look for us, and returning true
            // here would skip both.
            bool await_ready() const noexcept { return false; }

            // Runs on the SOURCE thread, before control leaves it.
            //
            // ORDER MATTERS: self_handle must be stored BEFORE post(). The
            // target's worker could dequeue and resume the node the instant
            // post() links it, and it reads self_handle without any further
            // synchronisation beyond the queue's own release/acquire pair.
            // Publishing the handle first is what makes that safe.
            //
            // Note this is the cross-thread variant of I2's cousin: no SQE is
            // created here, so there is no user_data to publish -- the queue
            // link IS the hand-off.
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

            // Returns nothing. A transfer has no failure mode: either the task
            // was enqueued and will resume on the target, or post() was never
            // reached. Errors belong to the operations performed after the
            // transfer, not to the transfer itself.
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
        ///
        /// One tick, no stop token, no teardown. This is the "drive this IO from
        /// a thread that is already running its own loop" entry point (sync_wait
        /// and the tests use it), not a replacement for run_blocking(): nothing
        /// here installs a stop callback, and nothing here drains, so the caller
        /// owns both.
        ///
        /// ORDER: owner_thread_ must be claimed BEFORE activate(), because
        /// activate() enables the ring that the loop then drives.
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
        ///
        /// Callable from ANY thread -- this is one of the two legal cross-thread
        /// entry points (the other is schedule_on / post). The task is moved,
        /// released, and queued; the target worker resumes it on its next tick.
        ///
        /// INVARIANT: after this returns, `task` is empty and its frame is
        /// owned by the scheduler (promise::detached == true). The caller must
        /// not keep or touch the coroutine afterwards.
        void schedule(Task<void> task)
        {
            // Admission gate. Once stop has been requested this IO accepts no
            // new work: teardown escalates to IORING_ASYNC_CANCEL_ANY, and a
            // task accepted during the drain could submit fresh I/O while
            // unwinding and hang again. Dropping here (rather than in post())
            // means the Task destructor owns the frame, as usual.
            //
            // Note the ordering: the gate is checked BEFORE release(). A refused
            // task is simply destroyed by this parameter's destructor, so it is
            // cleaned up rather than leaked. Checking after release() would
            // strand the frame with nobody holding it.
            //
            // Measured: this gate is free. Deleting it entirely made the server
            // measurably SLOWER in both build configurations (the load compiles
            // to a plain mov on x86-64, and the branch is perfectly
            // predicted), so there is no throughput argument for removing it.
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
        ///
        /// Acquire, and deliberately no stronger. This latch publishes no data,
        /// and the only thing the reader does with it is refuse work. The
        /// ordering that once mattered -- store-then-wake paired with
        /// check-then-park -- is gone with the wake channel, because the park is
        /// bounded and the worker re-reads this on its own every tick.
        [[nodiscard]] bool stopping() const noexcept
        {
            return stop_requested_.load(std::memory_order_acquire);
        }

        /// Awaitable form of the transfer. See TransferTo above, and note the
        /// one-way-door consequence under invariant I1.
        [[nodiscard]] static auto schedule_on(IO& target) noexcept { return TransferTo{target}; }

        /// Pin this thread to the CPU named by opts_.worker_cpu_affinity[id_].
        /// No-op when the list is empty or id_ is out of range. Called once by
        /// whichever thread claims ownership, before activate().
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

        // ============================================================================
        // Operation factories
        // ============================================================================
        //
        // Every method below has the same three-part shape and returns the same
        // IoAwaiter type; only the two lambdas differ:
        //
        //   setup   fills the SQE -- opcode, fd, buffers, flags
        //   mapper  turns the CQE's int32_t into a Result<T>
        //
        // Two rules apply to all of them, and both are easy to get wrong:
        //
        // 1. CAPTURE BY VALUE OR BY REFERENCE-TO-OWNER, NEVER A TEMPORARY.
        //    The lambda lives in the coroutine frame for as long as the
        //    operation is outstanding, which is longer than the full
        //    expression that created it. Catching `Fd&` by value (raw_fd) is
        //    the pattern used everywhere below; the few that genuinely need to
        //    reference the caller's object (accept's client_addr, connect's
        //    addr) say so in their own comments, and those objects MUST
        //    outlive the operation.
        //
        // 2. THE MAPPER OWNS THE SIGN CONVENTION.
        //    io_uring reports errors as negative errno and results as a
        //    non-negative value. detail::ResumeInt / detail::ResumeVoid /
        //    detail::ResumeFd handle it; a per-operation mapper exists only
        //    where the success value needs a different type (Fd, unsigned).
        //
        // [[nodiscard]] on all of them is deliberate and not decorative: the
        // returned awaiter is where the result lives, so discarding it silently
        // drops an I/O error.
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
                [path = std::move(path), flags, mode](io_uring_sqe* sqe)
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
                *this, [path = std::move(path)](io_uring_sqe* sqe)
                {
                    io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0);
                },
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

        // ============================================================================
        // Timers
        // ============================================================================
        //
        // These are KERNEL timers (IORING_OP_TIMEOUT), not userspace sleeps: the
        // worker is not blocked while one is outstanding, so a timeout costs an
        // SQE and a CQE like anything else.
        //
        // POLICY: timeouts belong in teardown, not in the steady-state loop.
        // The worker already parks efficiently inside
        // io_uring_submit_and_wait(); adding a wall-clock deadline to the hot
        // path converts that efficient block into a poll-until-deadline, which
        // is strictly worse. If you find yourself wanting a steady-state
        // timeout, the thing you actually want is a state machine that
        // co_awaits the operation and the timer in whichever order suits the
        // protocol, not a shortened park.
        //
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
                .tv_nsec = static_cast<long long>(duration_cast<nanoseconds>(dur - secs).count())
            };
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
        //
        // These wrap the low-level *_fixed overloads above with BufferLease
        // convenience, and -- more importantly -- with the one safety check the
        // raw versions cannot do: owns().
        //
        // INVARIANT: a fixed-buffer submission is only meaningful against a
        // buffer registered with THIS ring. buf_index is an index into this
        // IO's registration table; a lease from another worker's pool, or an
        // empty lease, has no valid index here, and submitting one would read
        // or write arbitrary memory inside the kernel.
        //
        // Rather than assert or throw, these helpers submit against fd -1 so
        // the operation completes with -EBADF. The mistake is then reported
        // through the normal Result<T> channel and the caller can handle it,
        // instead of corrupting memory. This is deliberate: the fixed-buffer
        // helpers exist precisely to avoid kernel copies, so failing loudly in
        // userspace would defeat their purpose, and failing silently would be
        // far worse.
        //
        // The BufferLease itself is captured as a raw pointer plus length, NOT
        // as the lease. The lease is a handle with lifetime semantics; the
        // kernel only needs the address, and the caller must keep the lease
        // alive until the operation completes (same rule as any buffer passed
        // to an async call).
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

        // ------------------------------------------------------------------------
        // Sentinel
        // ------------------------------------------------------------------------
        //
        // The one internal SQE is told apart from user operations by its
        // user_data. It is deliberately outside the range of any real &ops_
        // pointer: an IoOps* is always 8-byte aligned and heap-allocated, so
        // this unaligned, astronomically large value cannot collide with one.
        // That is what lets the CQ walk branch on the raw integer with no
        // registry.
        //
        // kCancelSentinel: our own IORING_ASYNC_CANCEL_ANY from teardown. Its
        // res is the number of operations matched, not an error. The operations
        // it cancelled have their own CQEs and resume their own coroutines.
        static constexpr uint64_t kCancelSentinel = 0xCACE'1ED0'CACE'1ED0ULL;

        // ---- Producer-visible state.
        //
        // The stop latch is the ONLY thing another thread writes. It sits on
        // its own cache line so that write cannot invalidate a line the owner
        // thread is cycling through every tick.
        //
        // One-shot stop latch, set by the stop callback installed in
        // run_blocking(). It publishes nothing -- no data is handed over
        // through it -- so it only has to be visible eventually, and acquire/
        // release is more than that requires. There is deliberately no longer a
        // handshake pairing it with the park: the park is bounded, so the
        // worker comes back on its own and reads the flag within one heartbeat
        // tick. seq_cst was only ever needed to make that handshake sound.
        std::atomic<bool> stop_requested_{false};

        // ---- Owner-thread state.
        //
        // Everything from here down is touched by the owner thread and nobody
        // else. That includes the kernel: io_uring_submit* and the CQ walk are
        // all owner-thread calls, which is what makes the non-atomic ring_
        // acceptable even though the kernel writes into its rings from the
        // kernel side.
        alignas(64) io_uring ring_{};

        // Rings are created with IORING_SETUP_R_DISABLED and enabled by
        // activate(). Two flags, both owner-thread only:
        //   is_activated_  the ring can accept SQEs
        //   is_running_    a loop is currently driving it. Checked by the move
        //                   ctor to reject moving a live IO.
        bool is_activated_{false};
        bool is_running_{false};
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
        // Cross-thread schedule()/post() stay safe: they only touch the
        // lock-free queue, never get_sqe().
        //
        // INVARIANT: non-atomic and owner-thread-only. It is written once at
        // the top of each entry point and read only from get_sqe() and the
        // move ctor, all of which run on the owning thread. There is no
        // happens-before edge needed because ownership transfer is established
        // by the thread creation/handoff that precedes the loop.
        std::thread::id owner_thread_{};

        // Resume staging. local_tasks_ accumulates handles to resume this tick
        // (drained from the MPSC queue, plus the coroutines woken by CQE
        // completions); current_batch is the working set being resumed right
        // now, so that iterating it stays valid while the resumes push new
        // handles back onto local_tasks_.
        //
        // Both are owner-thread only. local_tasks_ is pre-reserved to
        // opts_.entries in the constructor so steady-state pushes do not
        // reallocate; current_batch is pre-reserved to batch_max_size, the
        // most a single tick can ever stage.
        std::vector<std::coroutine_handle<>> local_tasks_{};
        std::vector<std::coroutine_handle<>> current_batch{};

        // The single inbound cross-thread channel. See detail/queue.hpp for the
        // algorithm and its stub-node invariant.
        detail::CoroQueue queue_{};

        std::optional<RegisteredBufferPool> buffer_pool_{std::nullopt};
        std::vector<iovec> registered_iovecs_{};
        // CQEs consumed off the ring so far. Every SQE the user writes produces
        // exactly one CQE, so (ktail - completed_count_) is the number of
        // operations the kernel still owns. SQEs not yet flushed are not in ktail;
        // outstanding_ops() adds them back via unflushed_sqes().
        // Derived from ring state rather than a per-op counter, so quiescence
        // costs one subtraction instead of a store on the I/O path.
        //
        // INVARIANT: non-atomic, owner-thread only, monotonically increasing.
        // It counts CQEs, including the cancel sentinel -- the kernel produces a
        // completion for the cancel-all just like any other SQE, so excluding it
        // here would break the arithmetic that outstanding_ops() depends on.
        std::uint64_t completed_count_{0};

        // SQ-full backpressure counter. Monotonic, never reset, and only read to
        // decide when to log (first event, then every 4096th). Odometer, not a
        // level.
        std::uint64_t sq_full_events_{0};

        // Current park deadline in microseconds. Starts at the heartbeat
        // interval and doubles towards max_idle_wait_us on ticks that find
        // nothing; any tick that does work snaps it back. Owner-thread only.
        std::uint32_t heartbeat_us_{0};

        /// Creates the Ring in an uninitialized way
        void init(int wq_fd = -1);
        /// Activate a disabled ring, MUST be called after init()
        /// Enable a ring created with IORING_SETUP_R_DISABLED. noexcept: callers
        /// include run_blocking()/run_once(), where a throw would terminate the
        /// worker. Failure is logged FATAL (aborts): the ring is unusable.
        void activate() noexcept;
        /// One iteration of the loop. @param park_timeout Overrides the
        ///        heartbeat deadline when non-null; teardown passes the time left
        ///        before its hard deadline so a stuck operation cannot wedge the
        ///        worker.
        void tick(__kernel_timespec* park_timeout = nullptr) noexcept;
        /// Graceful drain bounded by opts_.shutdown_grace_ms, escalating to
        /// IORING_ASYNC_CANCEL_ANY and then draining the cancellations.
        void drain_until_quiescent() noexcept;
        void submit_cancel_all() noexcept;
        /// SQEs prepared by get_sqe() but not yet flushed to the kernel tail.
        [[nodiscard]] std::uint32_t unflushed_sqes() const noexcept
        {
            return ring_.sq.sqe_tail - ring_.sq.sqe_head;
        }

        /// Operations in flight or queued: those flushed to the kernel and not
        /// yet completed, plus those still sitting unflushed in the SQ.
        /// Owner-thread only; no atomics.
        ///
        /// ktail is a free-running 32-bit counter that wraps, so the difference is
        /// taken in 32 bits; widening first would underflow after 2^32 operations.
        ///
        ///   outstanding = (ktail - completed_count_)   // the kernel's view
        ///               + unflushed_sqes()            // not yet visible to it
        ///
        /// Why both terms are needed:
        ///   * ktail - completed: every SQE yields exactly one CQE, so this is
        ///     "submitted and not yet reaped". CQEs the harvest has not reached
        ///     yet are counted here too, which is correct -- they are unreaped.
        ///   * + unflushed: SQEs sitting in userspace have not moved ktail yet,
        ///     but they are committed work and must be waited for.
        ///
        /// INVARIANT: this is the loop condition that makes teardown terminate.
        /// It is a lower bound on real outstanding work (it never over-counts),
        /// so reaching zero is sufficient -- though not necessary -- to conclude
        /// quiescence, which is why the drain also checks local_tasks_ and the
        /// CQ via has_pending_work().
        [[nodiscard]] std::uint64_t outstanding_ops() const noexcept
        {
            const auto submitted =
                static_cast<std::uint32_t>(*ring_.sq.ktail) - static_cast<std::uint32_t>(completed_count_);
            return std::uint64_t{submitted} + unflushed_sqes();
        }

        /// Anything left for teardown to wait on or run. Strictly a superset of
        /// outstanding_ops(): the ring can be quiet while coroutines are still
        /// waiting to be resumed, or while a producer's task is sitting in the
        /// MPSC queue. All four terms must be clear before teardown is done.
        [[nodiscard]] bool has_pending_work() const noexcept;

        /// Fetch an SQE, flushing once if the SQ is full.
        ///
        /// INVARIANT (I1): must be called on owner_thread_ only. Asserts.
        io_uring_sqe* get_sqe() noexcept;

        /// Count an SQ-full backpressure event. Logs the first and then every
        /// 4096th: a synchronous log write per event would stall the very loop
        /// that is already saturated.
        void note_sq_full() noexcept;

        int ring_fd() const noexcept { return ring_.ring_fd; }

        /// -------------------------------------------------------------------
        /// The cross-thread hand-off.
        /// -------------------------------------------------------------------
        ///
        ///   producer thread                worker thread
        ///   ---------------                ------------
        ///   queue_.enqueue(task)  ----.     (parked, bounded by heartbeat)
        ///                            |
        ///                            +------> park expires
        ///                                     next tick(): drain() -> resume
        ///
        /// That is the entire protocol. There is no second channel and nothing
        /// to keep in sync with the worker's state, because the worker cannot
        /// sleep past its deadline. A task enqueued while the worker is parked is
        /// simply picked up on the way back -- at worst one heartbeat tick late.
        ///
        /// queue_.enqueue()'s release store on prev->next is what makes the task
        /// visible to the worker's acquire load; see detail/queue.hpp.
        void post(detail::TaskPromiseBase* task) { queue_.enqueue(task); }
    };

    // ============================================================================
    // IoAwaiter Implementation
    // This must be defined after IO is fully defined to avoid incomplete type errors.
    // ============================================================================
    //
    // The submission half of the awaiter. Three exits, and the return value is
    // what distinguishes them -- this is the standard awaiter's contract, and it
    // is easy to misread here:
    //
    //   return h                  RESUME THE CALLER NOW. The coroutine never
    //                              suspends: await_resume() runs inline, in this
    //                              same stack frame, with ops_.res already set.
    //                              Used for both failure paths.
    //   return noop_coroutine()   SUSPEND. Control goes to the kernel; the CQE
    //                              handler in tick() is what resumes `h`, later.
    //
    // So a failed submission and a successful one share ONE resume path at the
    // await site: the caller sees a Result either way and has no idea which
    // happened. There is no separate "did it suspend" branch anywhere.
    //
    // INVARIANT: get_sqe() asserts I1 (single issuer) before touching the
    // ring. Every exit that reaches the ring is therefore owner-thread only --
    // which is also why the admission-gate load below is safe to keep
    // unguarded.
    template <typename SetupFunc, typename MapperFunc>
        requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
    template <typename Promise>
    std::coroutine_handle<> IoAwaiter<SetupFunc, MapperFunc>::await_suspend(std::coroutine_handle<Promise> h) noexcept
    {
        // Admission gate: no new operations once stop has been requested.
        // ECANCELED is the same result a cancelled in-flight op receives, so a
        // coroutine unwinding through teardown sees one consistent error.
        //
        // This is the second half of teardown's two-part gate (the first is in
        // schedule()). Without it, a coroutine already queued for shutdown could
        // submit fresh I/O after drain_until_quiescent() had decided it was
        // quiescent, and the ring would never come back.
        if (io_.stopping()) [[unlikely]]
        {
            ops_.res = -ECANCELED;
            return h;
        }

        // get_sqe() already tried one submit to make room, so a null here means
        // the ring is genuinely saturated. Report EAGAIN rather than spinning:
        // backpressure is the correct answer, and the caller's normal error
        // handling is far better than a loop that burns the worker trying.
        io_uring_sqe* sqe = io_.get_sqe();
        if (sqe == nullptr)
        {
            io_.note_sq_full();
            ops_.res = -EAGAIN;
            return h;
        }

        this->ops_.h = h;

        // Ordering: setup_ runs BEFORE user_data is set. Be aware this is
        // defensive, not currently required -- verified against the liburing
        // this tree fetches: there is no memset() anywhere in liburing.h, and
        // none of the prep helpers kio uses (read/write/send/accept/close/
        // timeout/poll_add/cancel64/read_fixed/write_fixed/readv/writev/
        // openat/unlinkat/renameat/fsync/fallocate/ftruncate/connect) assign
        // sqe->user_data. prep_cancel64 looks like the exception but stores its
        // target in sqe->addr, not sqe->user_data.
        //
        // So the reverse order would happen to work today. It is still wrong,
        // for two reasons:
        //   * io_uring_get_sqe() returns a RECYCLED SQE -- fields the current
        //     op does not set keep stale values from a previous one. Depending
        //     on that is depending on undefined field content.
        //   * the order silently requires every current AND future prep
        //     helper to leave user_data alone. Writing it last makes the
        //     publication correct by construction rather than by inspection of
        //     a third-party header.
        // INVARIANT: user_data is the LAST thing written to this SQE.
        setup_(sqe);

        // THE PUBLICATION (invariant I2). This address points into the
        // coroutine frame that is currently suspended on this very line. The
        // frame cannot die while it is suspended here, so the kernel always has
        // somewhere valid to write the result. No registry, no refcount.
        io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&ops_));

        // Suspend: from here, this function does not run again until tick()
        // harvests the CQE, writes ops_.res, and resumes h.
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
        ///
        /// `out` and `done` are references into the CALLER's sync_wait frame, not
        /// into this coroutine's. That is why they stay valid: sync_wait's frame
        /// is a live stack frame that outlives the tick loop below, whereas a
        /// capturing lambda's would not. The write of `done = true` is the
        /// handshake that ends the loop.
        template <typename T>
        Task<void> sync_wait_body(Task<T> t, std::optional<Result<T>>& out, bool& done)
        {
            out = co_await std::move(t);
            done = true;
            co_return {};
        }
    } // namespace detail

    /// Testing utility, block a coroutine until it is done
    ///
    /// Drives `io` on the CALLING thread until `task` completes, then returns its
    /// Result. There is no event loop behind this and no other thread: sync_wait
    /// IS the tick() loop, inline.
    ///
    /// INVARIANT: this claims ring ownership for its duration, so it must not
    /// run concurrently with run_blocking()/run_once() on the same IO. It is the
    /// "main()" shape: construct the IO, sync_wait, done.
    ///
    /// Two subtleties worth naming:
    ///   * schedule() is called BEFORE owner_thread_ is claimed. That is
    ///     correct -- schedule() only enqueues, and never touches the ring
    ///     (see I1).
    ///   * owner_thread_ is claimed before activate(), because activate()
    ///     enables the ring that the loop then drives.
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

        // No stop check and no timeout: this terminates because the coroutine
        // completes, not because a deadline passes. sync_wait is a testing and
        // bootstrap utility -- putting a clock on it would only hide a hang.
        while (!done)
        {
            io.tick();
        }

        // INVARIANT: `done` is assigned only after `result` was, so the
        // dereference cannot hit an empty optional.
        return std::move(*result);
    }
} // namespace kio
