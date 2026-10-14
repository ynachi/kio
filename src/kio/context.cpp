#include "kio/context.h"
#include <absl/log/check.h>
#include <absl/log/log.h>
#include <sys/eventfd.h>

namespace kio::io
{
    void Context::init(const int wq_fd, const std::optional<BufferPoolConfig>& pool_cfg)
    {
        if (pool_cfg.has_value())
        {
            buffer_pool_.emplace(pool_cfg->slot_size, pool_cfg->slots);
            buffer_pool_->set_wake(
                this, [](void* ctx, std::coroutine_handle<> h) noexcept { static_cast<Context*>(ctx)->post(h); });
            buffer_pool_->append_regions(registered_iovecs_);
        }

        io_uring_params params{};

        // disable the ring first
        //
        // IORING_SETUP_R_DISABLED creates a ring that exists but cannot
        // submit. The reason that matters is issuer identity: with
        // SINGLE_ISSUER, submit rights are pinned to one thread. The
        // "owner" is the thread calling activate().
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
            // bare "Invalid argument" names neither the knob.
            throw make_context_error("io_uring_queue_init_params", rc);
        }

        LOG(INFO) << "Initialized IO context with " << opts_.entries
                  << " entries (SQPOLL: " << (opts_.flags & IORING_SETUP_SQPOLL ? "enabled" : "disabled") << ")";
    }

    Context::Context(const size_t id, const Context* leader, const IoOptions& opts,
                     const std::optional<BufferPoolConfig>& pool_cfg)
        : opts_(opts), id_(id)
    {
        CHECK(opts_.batch_max_size > 0) << "batch_max_size must be positive";
        if (pool_cfg.has_value())
        {
            CHECK(pool_cfg->slot_size >= 4096 && (pool_cfg->slot_size & (pool_cfg->slot_size - 1)) == 0 &&
                (RegisteredBufferPool::kOneGiB % pool_cfg->slot_size) == 0)
            << "BufferPoolConfig.slot_size must be a power of two >= 4096 dividing 1 GiB, got "
            << pool_cfg->slot_size;
        }
        // When share_work_queues is false, every worker builds a fully
        // independent ring instead of attaching to the leader's worker pool.
        int leader_fd = -1;
        if (leader != nullptr && opts_.share_work_queues)
        {
            leader_fd = leader->ring_fd();
        }
        init(leader_fd, pool_cfg);
    }

    void Context::run(const std::stop_token& st) noexcept
    {
        // A previous session may have been cut short by cancel_all();
        // a new run() is a fresh session, so linked-timeout ops report
        // ETIMEDOUT again instead of cancellation.
        canceling_all_ = false;
        activate();
        // Bind/debug-CHECK the loop-thread contract: everything below
        // (and every post/note_park from primitives) assumes one driver.
        check_loop_thread();

        if (st.stop_possible() && !wake_fd_.valid())
        {
            wake_fd_.reset(::eventfd(0, EFD_CLOEXEC)); // blocking on purpose
            CHECK(wake_fd_.valid()) << "eventfd failed: " << std::strerror(errno);
        }

        // Runs synchronously on the REQUESTER's thread. Captures an int, not `this`:
        // the foreign thread never touches the Context. Destroyed at the end of
        // run(); wake_fd_ is a member, so it outlives the callback.
        std::stop_callback wake_on_stop(st, [fd = wake_fd_.get()]() noexcept
        {
            constexpr std::uint64_t one = 1;
            (void)!::write(fd, &one, sizeof one);
        });

        constexpr std::uint64_t kChunk = 256;
        std::coroutine_handle<> chunk[kChunk];
        // Stop-observation tick: how often a blocked wait re-arms. The
        // in-kernel deadline is the ONLY sanctioned cross-thread wake --
        // no waker fd, ever (sharding.md §6). Pinned by the shutdown tests;
        // deliberately not a knob: one stop mode, no knobs. Non-const: the
        // pinned liburing's wait_timeout takes a plain __kernel_timespec*.
        static __kernel_timespec kStopPoll{.tv_sec = 0, .tv_nsec = 50'000'000};

        for (;;)
        {
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
                    if (i + kResumePrefetchDist < n)
                    {
                        // Warm the next frame's header line (resume/destroy
                        // fn pointers, promise) while this body runs.
                        __builtin_prefetch(chunk[i + kResumePrefetchDist].address(), 0, 3);
                    }
                    // may spawn / nested-reap -> next epoch
                    chunk[i].resume();
                }
            }

            // The ONE token read that drives behavior: stop observed on the
            // loop thread latches the force-drain. Everything downstream
            // (fail-fast staging in await_suspend, the bounded wait, the
            // ECANCELED classification) is driven by canceling_all_, never
            // by the token again. Armed unconditionally -- with nothing in
            // flight the cancel SQE completes -ENOENT under kIgnoreTag.
            if (!canceling_all_ && st.stop_requested())
            {
                cancel_all();
            }

            // The ONLY exit: quiesced. No stop condition here -- stop means
            // "drain to quiesce", never "leave early". Suppressed when stop
            // is pending (the old guard), this branch deadlocked: it fell
            // into submit_and_wait with zero outstanding ops, and no CQE
            // could ever arrive.
            if (ready_list_.empty() && inflight_ == 0)
            {
                CHECK((!buffer_pool_ || buffer_pool_->waiting() == 0) && external_parks_ == 0)
                    << "parked waiter with no one left to wake it "
                       "(pool leases leaked? baton never posted? semaphore never released?)";

                io_uring_submit(&ring_); // flush a just-armed cancel; empty SQ is a ring check

                return;
            }

            // Phase 2: submit, and block for a CQE only when nothing is
            // runnable. The unbounded wait is legal only when no stop can
            // EVER be observed (default token, or every stop_source
            // destroyed unrequested): otherwise the deadline is the wake.
            // canceling_all_ -- not the token -- gates the unbounded branch,
            // so a latched drain never sleeps unbounded.
            int r = -1;
            if (!ready_list_.empty())
            {
                r = io_uring_submit_and_get_events(&ring_);
            }
            else if (!canceling_all_)
            {
                if (st.stop_possible())
                {
                    arm_wake();
                }
                r = io_uring_submit_and_wait(&ring_, 1);
            }
            else
            {
                io_uring_cqe* cqe = nullptr; // peek only; reap() is the sole consumer
                r = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &kStopPoll, nullptr);
                if (r == -ETIME)
                {
                    // Deadline is the normal idle wake: re-check above.
                    // (liburing passes the kernel's EXT_ARG -ETIME through.)
                    r = 0;
                }
            }
            if (r < 0 && r != -EINTR && r != -EBUSY)
            {
                LOG(FATAL) << "fatal, io submit failed: " << r;
            }
            // reap. Only queues handles into the ready list, never resumes.
            reap();
        }
    }

    // Submit everything queued. EINTR: retry. EBUSY/EAGAIN: drain once, retry once.
    // Anything else is a broken ring.
    void Context::submit_or_die() noexcept
    {
        bool drained = false;
        for (;;)
        {
            const int r = io_uring_submit(&ring_);
            if (r >= 0) return;
            if (r == -EINTR) continue;
            if ((r == -EBUSY || r == -EAGAIN) && !drained)
            {
                drained = true;
                io_uring_get_events(&ring_); // flush CQ overflow, run deferred task work
                reap(); // only queues handles; never resumes
                continue;
            }
            LOG(FATAL) << "io submit failed: " << std::strerror(-r) << " (" << r << ")";
        }
    }

    io_uring_sqe* Context::get_sqe(const unsigned reserve) noexcept
    {
        if (io_uring_sq_space_left(&ring_) < reserve)
        {
            submit_or_die();
        }
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        CHECK(sqe != nullptr) << "SQ still full after submit: reserve=" << reserve
                              << " exceeds ring size, or re-entrancy bug";
        return sqe;
    }

    void Context::reap() noexcept
    {
        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned n = 0;
        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            ++n;
            const std::uint64_t ud = io_uring_cqe_get_data64(cqe);

            if (ud == detail::kWakeTag)
            {
                wake_armed_ = false;
                CHECK(cqe->res >= 0 || cqe->res == -ECANCELED)
                    << "wake eventfd read failed: " << std::strerror(-cqe->res);
                continue;
            }

            if (ud & detail::kIgnoreTag)
            {
                continue;
            }

            auto* op = reinterpret_cast<detail::IoAwaitableBase*>(ud);
            CHECK(op->lifecycle() == detail::IoAwaitableState::Inflight) << "CQE for an op that is not in flight";
            op->res = cqe->res;
            op->set_lifecycle(detail::IoAwaitableState::Done);
            op_finished();
            post(op->handle);
        }
        io_uring_cq_advance(&ring_, n);
    }

    void Context::cancel(detail::IoAwaitableBase& op) noexcept
    {
        // One cancel SQE per op: a repeat would submit a second
        // IGNORE_TAG SQE for nothing, and re-flagging is meaningless.
        if (op.lifecycle() != detail::IoAwaitableState::Inflight || op.cancel_requested())
        {
            return;
        }

        op.set_cancel_requested();
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, &op, 0);
        io_uring_sqe_set_data64(sqe, detail::kIgnoreTag);
    }

    void Context::activate() noexcept
    {
        // calling wait is idempotent
        if (is_activated_)
        {
            LOG(WARNING) << "Context::activate() called twice";
            return;
        }

        // Enables the ring and records THIS thread as the single issuer:
        // after activate() every submit must come from this thread.
        if (const int ret =
                io_uring_register(static_cast<unsigned>(ring_.ring_fd), IORING_REGISTER_ENABLE_RINGS, nullptr, 0);
            ret < 0)
        {
            LOG(FATAL) << "io_uring_register failed: " << std::strerror(-ret);
        }

        // Register the buffer pool table now that the ring is live.
        // Fixed-buffer SQEs are unusable before this; nothing can submit
        // before activate() returns.
        if (!registered_iovecs_.empty())
        {
            if (const int rc = io_uring_register_buffers(&ring_, registered_iovecs_.data(),
                                                         static_cast<unsigned>(registered_iovecs_.size()));
                rc < 0)
            {
                LOG(FATAL) << "io_uring_register_buffers failed: " << std::strerror(-rc) << " ("
                       << registered_iovecs_.size() << " regions, " << buffer_pool_->allocated_bytes()
                       << " bytes pinned)"
                       << (rc == -ENOMEM
                               ? "; check `ulimit -l` (RLIMIT_MEMLOCK) or LimitMEMLOCK= in the unit file"
                               : "");
            }
        }

        is_activated_ = true;
    }

    void Context::arm_wake() noexcept
    {
        if (wake_armed_)
        {
            return;
        }
        io_uring_sqe* s = get_sqe();
        io_uring_prep_read(s, wake_fd_.get(), &wake_buf_, sizeof wake_buf_, ~0ULL);
        io_uring_sqe_set_data64(s, detail::kWakeTag);
        wake_armed_ = true;
    }
} // namespace kio::io
