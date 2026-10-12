#pragma once

#include <algorithm>
#include <coroutine>
#include <cstdlib>
#include <cstring>
#include <liburing.h>

#include "buffer_pool.hpp"
#include "detail/awaitable.h"
#include "detail/ready_ring.h"
#include "absl/log/check.h"
#include "absl/log/log.h"

namespace kio
{
    template <class T>
    class Task;
}

namespace kio::io
{
    struct IoAwaitableBase;

    static constexpr unsigned kUringDefaultFlag =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    static constexpr int kDefaultAcceptFlags = SOCK_NONBLOCK | SOCK_CLOEXEC;

    struct IoOptions
    {
        std::uint32_t entries = 4096;
        unsigned flags = kUringDefaultFlag;

        bool share_work_queues = true;

        // list of cpus, if empty, no pinning
        std::vector<int> worker_cpu_affinity{};

        // Max coroutine resumes per run-loop epoch (the Phase 1 batch cap).
        std::size_t batch_max_size = 128;

        std::uint32_t heartbeat_interval_us = 50;

        std::uint32_t max_idle_wait_us = 2000;

        std::uint32_t shutdown_grace_ms = 5000;

        std::uint32_t sq_thread_idle_ms = 2000;
        // -1 means don't pin to a specific CPU
        int sq_thread_cpu = -1;
    };

    class Context
    {
        template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep>
        friend class IoAwaitable;

        friend void cancel(Context&, IoAwaitableBase&) noexcept;


        int ring_fd() const noexcept { return ring_.ring_fd; }

        void init(const int wq_fd)
        {
            // PORTABILITY NOTE on flag exclusivity, verified empirically on
            // kernel 7.0.0-38 (raw io_uring_setup, all combos create and run
            // ops): SQPOLL+DEFER_TASKRUN and DEFER_TASKRUN-without-
            // SINGLE_ISSUER are rejected with EINVAL on kernels 5.19-6.x but
            // accepted on 7.0+. Do not pre-validate these here -- the kernel
            // is the authority and its error (with the flags printed below)
            // is the diagnosis. If old-kernel support ever matters, probe at
            // runtime instead of hardcoding version rules.
            io_uring_params params{};

            // disable the ring first
            //
            // IORING_SETUP_R_DISABLED creates a ring that exists but cannot
            // submit. The reason that matters is issuer identity: with
            // SINGLE_ISSUER, submit rights are pinned to one thread, and for
            // a live ring that thread is the one that CREATED it -- the
            // constructor's thread. Construct-on-main, run-on-worker would
            // fail on the first submit. A disabled ring instead records the
            // issuer when it is enabled, which activate() does at the top of
            // run(), on the thread that will actually drive the loop. It
            // also gives us a fully-formed object -- wake fd, room for
            // buffer registration -- before any SQE may flow.
            params.flags = opts_.flags | IORING_SETUP_R_DISABLED;

            // attach WQ
            if (wq_fd >= 0)
            {
                params.flags |= IORING_SETUP_ATTACH_WQ;
                params.wq_fd = static_cast<__u32>(wq_fd);
            }

            // Safely configure SQPOLL if requested
            if (params.flags & IORING_SETUP_SQPOLL)
            {
                params.sq_thread_idle = opts_.sq_thread_idle_ms;
                if (opts_.sq_thread_cpu >= 0)
                {
                    params.flags |= IORING_SETUP_SQ_AFF;
                    params.sq_thread_cpu = static_cast<__u32>(opts_.sq_thread_cpu);
                }
            }

            if (const int rc = io_uring_queue_init_params(opts_.entries, &ring_, &params); rc < 0)
            {
                // Flags in hex: flag-pair rejections (e.g. SQPOLL+DEFER_TASKRUN
                // on kernels 5.19-6.x) are kernel-version-dependent, and the
                // bare "Invalid argument" names neither knob.
                throw std::runtime_error(std::format(
                    "io_uring_queue_init_params failed: {} (flags: {:#x}, entries: {})",
                    std::strerror(-rc), opts_.flags, opts_.entries));
            }

            // TODO: register buffer

            LOG(INFO) << "Initialized IO context with " << opts_.entries
                      << " entries (SQPOLL: "
                      << (opts_.flags & IORING_SETUP_SQPOLL ? "enabled" : "disabled")
                      << ")";
        }

    public:
        explicit Context(const size_t id, const Context* leader = nullptr,
                         const IoOptions& opts = {}) : opts_(opts), id_(id)
        {
            CHECK(opts_.batch_max_size > 0) << "batch_max_size must be positive";
            // init. When share_work_queues is false, every worker builds a fully
            // independent ring instead of attaching to the leader's worker pool.
            int leader_fd = -1;
            if (leader != nullptr && opts_.share_work_queues)
            {
                leader_fd = leader->ring_fd();
            }
            init(leader_fd);
        }

        ~Context()
        {
            // CHECK, not assert: these are exactly the states where
            // continuing would corrupt or leak, and assert compiles out of
            // release builds -- this library aborts at the bug site.
            CHECK(inflight_ == 0) << "Context destroyed with operations still in flight";
            // run() drains ready_list_ before returning, so this is only
            // reachable when spawn() was called and run() never ran (or is
            // not running): the queued frames would leak without this.
            CHECK(ready_list_.empty()) << "Context destroyed with unstarted work (spawn() without run())";
            io_uring_queue_exit(&ring_);
        }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        // TODO: define the move ctor
        Context& operator=(Context&&) = delete;

        template <class T>
        void spawn(Task<T> task) noexcept;

        // Runs until there is no ready coroutine and no in-flight op.
        void run() noexcept
        {
            // A previous session may have been cut short by cancel_all();
            // a new run() is a fresh session, so linked-timeout ops report
            // ETIMEDOUT again instead of cancellation.
            canceling_all_ = false;
            activate();

            // Phase 1 pops through this fixed stack buffer so the epoch cap
            // never costs a variable-size frame, and the batch is owned
            // memory: resumes run user code that may push (spawn, nested
            // reap) and even grow-and-rebase the ring underneath us.
            constexpr std::uint64_t kChunk = 256;
            std::coroutine_handle<> chunk[kChunk];

            for (;;)
            {
                // Phase 1: at most batch_max_size resumes this epoch. The cap
                // bounds the time between kernel submits when the ready list
                // keeps refilling (spawn storms, nested reaps); a backlog is
                // drained over successive epochs, FIFO.
                std::uint64_t remaining = opts_.batch_max_size;
                while (remaining > 0)
                {
                    const std::uint64_t n = ready_list_.pop_batch(chunk, std::min(kChunk, remaining));
                    if (n == 0)
                    {
                        break;
                    }
                    remaining -= n;
                    for (std::uint64_t i = 0; i < n; ++i)
                    {
                        if (i + 2 < n)
                        {
                            // Warm the next frame's header line (resume/destroy
                            // fn pointers, promise) while this body runs.
                            __builtin_prefetch(chunk[i + 2].address(), 0, 3);
                        }
                        chunk[i].resume(); // may spawn / nested-reap -> next epoch
                    }
                }

                if (ready_list_.empty() && inflight_ == 0)
                {
                    io_uring_submit(&ring_); // flush stray SQEs
                    return;
                }

                // Phase 2: submit, and block for a CQE only when nothing is
                // runnable. Parking a non-empty ready list behind a wait would
                // invert the point of the epoch cap; with an empty SQ, submit
                // is a ring check without a syscall, so the busy path is free.
                const int r = ready_list_.empty()
                                  ? io_uring_submit_and_wait(&ring_, 1)
                                  : io_uring_submit(&ring_);
                if (r < 0 && r != -EINTR && r != -EBUSY)
                {
                    LOG(FATAL) << "fatal, io submit failed: " << r;
                    std::abort();
                }
                // Phase 3: reap. Only queues handles into the ready list, never resumes.
                reap();
            }
        }

        /// @brief Query the registered buffer pool, if configured.
        [[nodiscard]] const std::optional<RegisteredBufferPool>& buffer_pool() const noexcept
        {
            return buffer_pool_;
        }

        // typically called on teardown
        void cancel_all() noexcept
        {
            // Latched so ops completed by this cancel report cancellation
            // instead of a bogus ETIMEDOUT (their await_resume cannot tell a
            // teardown cancel from a fired linked timeout otherwise).
            // Cleared at the top of the next run().
            canceling_all_ = true;
            io_uring_sqe* s = get_sqe();
            io_uring_prep_cancel64(s, 0, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY);
            io_uring_sqe_set_data64(s, IGNORE_TAG);
        }

    private:
        // ---- Hot: touched by post/reap/resume on every completion. Kept
        // adjacent so the loop's working set covers as few cache lines as
        // possible; everything configuration-shaped lives below the cold
        // marker. ----
        detail::ReadyRing ready_list_{}; // Growing pow2 ring; the loop's runnable queue (see detail/ready_ring.h)
        io_uring ring_{};
        uint64_t inflight_{};
        bool is_activated_{false};
        // Set by cancel_all(), read by IoAwaitable::await_resume so ops
        // canceled by a teardown report -ECANCELED instead of a fake
        // ETIMEDOUT. Loop-thread contract applies to writes too: cancel_all()
        // prepares an SQE, so calling it while run() is live on another
        // thread is the same pre-existing cross-thread hazard as any
        // submission.
        bool canceling_all_{false};

        // ---- Cold: construction config and rare accessors. ----
        std::optional<RegisteredBufferPool> buffer_pool_{std::nullopt};
        unsigned setup_flags_ = 0;
        std::vector<iovec> registered_iovecs_{};
        IoOptions opts_;
        size_t id_;

        //
        // NEW METHODS, v2 migration
        //
        io_uring_sqe* get_sqe(const unsigned reserve = 1) noexcept
        {
            if (io_uring_sq_space_left(&ring_) < reserve)
            {
                if (const int r = io_uring_submit(&ring_); r == -EBUSY)
                {
                    // The CQ ring overflowed: the kernel takes no new work
                    // until completions are drained. reap() is loop-thread
                    // state, and the coroutine calling get_sqe is running,
                    // so its own op cannot appear in the drained batch.
                    reap();
                    io_uring_submit(&ring_);
                }
            }
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);

            if (sqe == nullptr)
            {
                // Only reachable if the SQ is genuinely full after the
                // drain above: reserve larger than the SQ, or a re-entrancy
                // bug somewhere in the loop.
                LOG(ERROR) << "urco: submission queue full";
                std::abort();
            }

            return sqe;
        }

        void op_started() noexcept { ++inflight_; }

        void op_finished() noexcept { --inflight_; }

        // Queue a coroutine handle to be resumed by the run loop.
        // Loop-thread only: ready_list_ is a plain member and every mutator
        // (post, run, reap) executes on the run-loop thread.
        //
        // A node must not be queued twice, and by construction it cannot be:
        // ops are one-shot and reap() CHECKs each CQE landed on an op still
        // in flight, await_suspend() rejects a second awaiter before any CQE
        // exists, and spawn() posts a fresh frame every call. The two-op-one-
        // handle case that WOULD break this (a future when_any handing one
        // handle to two ops) belongs guarded in that combinator.
        void post(const std::coroutine_handle<> handle) noexcept
        {
            CHECK(handle.address() != nullptr) << "post of a null coroutine handle";
            // Note: ReadyRing::push allocates only when it grows; steady
            // state never allocates (the ring drains to empty at every phase
            // boundary, so capacity stays warm). A grow that throws
            // std::bad_alloc inside this noexcept context calls
            // std::terminate(), which matches this library's error handling
            // philosophy.
            ready_list_.push(handle);
        }

        void reap() noexcept
        {
            io_uring_cqe* cqe = nullptr;
            unsigned head = 0;
            unsigned n = 0;
            io_uring_for_each_cqe(&ring_, head, cqe)
            {
                ++n;
                const std::uint64_t ud = io_uring_cqe_get_data64(cqe);
                if (ud & IGNORE_TAG) continue; // link-timeout / cancel CQE
                auto* op = reinterpret_cast<IoAwaitableBase*>(ud);
                // One-shot ops produce exactly one CQE; a second one here
                // would resume the coroutine twice. Inflight is the only
                // state a CQE may legally arrive in -- a canceled op is still
                // Inflight (cancel is a flag, not a state), so this CHECK is
                // airtight for re-reaped batches too.
                CHECK(op->lifecycle() == IoAwaitableState::Inflight)
                    << "CQE for an op that is not in flight";
                op->res = cqe->res;
                op->set_lifecycle(IoAwaitableState::Done);
                op_finished();
                post(op->handle);
                // `op` may be destroyed as soon as its coroutine runs: don't touch it again.
            }
            io_uring_cq_advance(&ring_, n);
        }

        void cancel(IoAwaitableBase& op) noexcept
        {
            // One cancel SQE per op: a repeat would submit a second
            // IGNORE_TAG SQE for nothing, and re-flagging is meaningless.
            if (op.lifecycle() != IoAwaitableState::Inflight || op.cancel_requested())
                return;

            op.set_cancel_requested();
            io_uring_sqe* sqe = get_sqe();
            io_uring_prep_cancel(sqe, &op, 0);
            io_uring_sqe_set_data64(sqe, IGNORE_TAG);
        }

        void activate() noexcept
        {
            if (is_activated_)
            {
                LOG(INFO) << "IO is already activated, this is a noop";
                return;
            }

            // Enables the ring and records THIS thread as the single issuer:
            // after activate() every submit must come from this thread. It
            // runs at the top of run(), so the binding is "the thread that
            // called run() first" -- call run() from one thread only.
            // Not throwing: run_blocking() and run_once() are both noexcept. A ring that
            // cannot be enabled is unusable, so this is logged FATAL (which aborts).
            if (const int ret = io_uring_register(static_cast<unsigned>(ring_.ring_fd),
                                                  IORING_REGISTER_ENABLE_RINGS, nullptr, 0); ret < 0)
            {
                LOG(FATAL) << "io_uring_register failed: " << std::strerror(-ret);
            }

            is_activated_ = true;
        }
    };

    template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep>
    void IoAwaitable<Prep>::await_suspend(
        const std::coroutine_handle<> h) noexcept
    {
        // One await per op: state leaves Created exactly once, and a second
        // co_await (state Inflight or Done) would strand the first waiter or
        // double-consume the result.
        CHECK(lifecycle() == IoAwaitableState::Created) << "IoAwaitable awaited twice";
        set_lifecycle(IoAwaitableState::Inflight);
        handle = h;
        io_uring_sqe* sqe = io_.get_sqe(has_link_timeout() ? 2 : 1);
        prep_(sqe, &scratch_);
        io_uring_sqe_set_data(sqe, static_cast<IoAwaitableBase*>(this));
        if (has_link_timeout())
        {
            sqe->flags |= IOSQE_IO_LINK;
            io_uring_sqe* t = io_.get_sqe(1);
            io_uring_prep_link_timeout(t, &link_ts_, 0);
            io_uring_sqe_set_data64(t, IGNORE_TAG);
        }

        io_.op_started();
    }

    template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep>
    Result<int> IoAwaitable<Prep>::await_resume() const noexcept
    {
        if (res >= 0)
        {
            return res;
        }
        // TODO: we pay for this if for each non time based ops. Something to do here ?
        if (res == -ETIME && etime_is_success())
        {
            return 0;
        }
        // A fired link-timeout cancels the primary op, which surfaces here as
        // -ECANCELED while the op is Done and unflagged: that IS a timeout.
        // An explicit cancel(op) sets cancel_requested, and a teardown-wide
        // cancel_all() latches Context::canceling_all_ -- those are genuine
        // cancellations and must not masquerade as ETIMEDOUT.
        if (res == -ECANCELED && has_link_timeout() && !cancel_requested() && !io_.canceling_all_)
        {
            return Error::fail_errno(ETIMEDOUT, "operation timed out");
        }
        if (res == -ECANCELED)
        {
            return Error::fail_errno(-res, "operation canceled");
        }
        // TODO: if we give a name to the operation, we can use it here. For instance write, read, ...
        // To do this, we could add a member to the awaitable base and set it on construction
        return Error::fail_errno(-res, "io operation failed");
    }
} // namespace kio::io
