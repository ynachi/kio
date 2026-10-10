// ============================================================================
// kio::IO -- implementation
// ============================================================================
//
// The invariants I1 (single issuer), I2 (the frame is the operation table) and
// I3 (the park is bounded), the tick() phase list and the two-phase teardown
// are all documented in include/kio/core/io.h. Read that header first; this
// file does not repeat them, it points at them and covers only what is
// specific to the implementation.
//
// INVARIANT ENFORCED THROUGHOUT: everything in here runs on the owner thread
// EXCEPT the stop callback installed by run_blocking() and the producers that
// call post(). That is why nearly every field touched below is non-atomic, and
// why the only atomics in the file are stop_requested_ and the queue's head_.
//
// One implementation note worth stating up front, because it looks like a bug
// otherwise: io_uring_submit_and_wait*() returns the NUMBER OF SQEs THE KERNEL
// CONSUMED, which is positive on success. Code that writes `if (rc == 0) fail`
// treats every successful park as an error. Only a negative return is a failure.
//
// Also: liburing writes through the cqe_ptr argument unconditionally, so the
// out-parameter cannot be null even when the caller only cares about the wait.
// ============================================================================

#include "kio/core/io.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <future>
#include <iterator>

#include <unistd.h>

#include "kio/core/awaiter.hpp"

namespace kio
{
    IO::IO(const size_t id, const IO* leader, const IoOptions& opts, const std::optional<BufferPoolConfig> pool_cfg)
        : opts_(opts), id_(id)
    {
        opts_.batch_max_size = std::max<std::size_t>(1, opts_.batch_max_size);
        opts_.heartbeat_interval_us = std::max<std::uint32_t>(1, opts_.heartbeat_interval_us);
        opts_.max_idle_wait_us = std::max(opts_.max_idle_wait_us, opts_.heartbeat_interval_us);
        local_tasks_.reserve(opts_.entries);
        current_batch.reserve(opts_.batch_max_size);
        heartbeat_us_ = opts_.heartbeat_interval_us;

        if (pool_cfg.has_value())
        {
            buffer_pool_.emplace(pool_cfg->slot_size, pool_cfg->slots);

            // The pool's backpressure queue has no idea which IO it belongs to,
            // so it is handed a type-erased resume callback at construction:
            // when a lease frees a slot, every coroutine waiting on that slot
            // is queued to be resumed. The queue stores coroutine_handles
            // rather than promises because the buffer pool is deliberately
            // unaware of the coroutine machinery -- it only knows "something
            // is waiting for this slot now".
            //
            // The hook is re-installed after a move, because it captures `this`
            // and the moved-to IO is a different address.
            buffer_pool_->set_reschedule_hook(
                this,
                [](void* ctx, const std::coroutine_handle<> h) noexcept
                {
                    static_cast<IO*>(ctx)->local_tasks_.push_back(h);
                });
        }

        // init. When share_work_queues is false, every worker builds a fully
        // independent ring instead of attaching to the leader's worker pool.
        int leader_fd = -1;
        if (leader != nullptr && opts_.share_work_queues)
        {
            leader_fd = leader->ring_fd();
        }
        init(leader_fd);
    }

    IO::IO(IO&& other) noexcept
        : stop_requested_(other.stop_requested_.load(std::memory_order_acquire)),
          is_activated_(other.is_activated_),
          is_running_(other.is_running_),
          opts_(other.opts_),
          id_(other.id_),
          buffer_pool_(std::move(other.buffer_pool_)),
          registered_iovecs_(std::move(other.registered_iovecs_))
    {
        // Safety check, we SHOULD not move a running IO. An activated one is not
        // inert either: its ring has already been handed to the kernel, and any
        // SQE prepared against it points at the SOURCE object's storage.
        //
        // completed_count_ and heartbeat_us_ are deliberately not copied: they
        // describe a ring's history, and that history does not follow a
        // byte-copy of the descriptor.
        //
        // Everything else here is a genuine transfer: the source is zeroed so
        // its destructor cannot exit a ring this object now owns.
        if (is_running_ || is_activated_ || !other.queue_.empty() || !other.local_tasks_.empty())
        {
            KIO_LOG_FATAL("FATAL: IO move failed. IO must be inert (not activated, not running, empty) to move.");
            std::terminate();
        }

        if (buffer_pool_.has_value())
        {
            buffer_pool_->set_reschedule_hook(
                this,

                [](void* ctx, std::coroutine_handle<> h) noexcept
                {
                    static_cast<IO*>(ctx)->local_tasks_.push_back(h);
                });
        }

        // Transfer Ring
        ring_ = other.ring_;
        std::memset(&other.ring_, 0, sizeof(io_uring));
        other.ring_.ring_fd = -1;
    }

    void IO::init(const int wq_fd)
    {
        io_uring_params params{};

        // disable the ring first
        //
        // IORING_SETUP_R_DISABLED creates a ring that exists but cannot submit.
        // That gives us a fully-formed io_uring object -- including the wake
        // fd and the registered buffer pool -- before the owner thread is known
        // and before we are ready to have SQEs flow. activate() is what enables
        // it. The buffer registration below is the reason this ordering is
        // more than tidiness: registering against a live ring would open a
        // window in which another thread could submit through it.
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
            throw std::runtime_error(std::format("io_uring_queue_init_params failed: {}", std::strerror(-rc)));
        }

        // register buffer pool if configured
        if (buffer_pool_.has_value())
        {
            registered_iovecs_.clear();
            buffer_pool_->append_regions(registered_iovecs_);

            if (const int ret = io_uring_register_buffers(
                    &ring_, registered_iovecs_.data(), static_cast<unsigned>(registered_iovecs_.size()));
                ret < 0)
            {
                KIO_LOG_ERROR("failed to register buffer pool: {}", std::strerror(-ret));
                io_uring_queue_exit(&ring_);
                // The constructor is about to throw, so ~IO() will not run.
                throw std::runtime_error("io_uring_register_buffers failed");
            }
            KIO_LOG_INFO("Worker {} registered {} MB buffer pool ({} x 1GB chunks)",
                         id_, buffer_pool_->total_bytes() / (1024 * 1024),
                         static_cast<unsigned>(registered_iovecs_.size()));
        }

        KIO_LOG_INFO("Initialized IO context with {} entries (SQPOLL: {})", opts_.entries,
                     (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
    }

    void IO::activate() noexcept
    {
        if (is_activated_)
        {
            KIO_LOG_DEBUG("IO is already activated, this is a noop");
            return;
        }

        // Not throwing: run_blocking() and run_once() are both noexcept. A ring that
        // cannot be enabled is unusable, so this is logged FATAL (which aborts).
        if (const int ret = io_uring_register(static_cast<unsigned>(ring_.ring_fd),
                                              IORING_REGISTER_ENABLE_RINGS, nullptr, 0); ret < 0)
        {
            KIO_LOG_FATAL("io_uring_register failed: {}", std::strerror(-ret));
        }

        is_activated_ = true;
    }

    void IO::tick(__kernel_timespec* park_timeout) noexcept
    {
        // 1. Drain the cross-thread MPSC queue.
        queue_.drain(
            [this](detail::TaskPromiseBase* node)
            {
                // We retrieve the safe handle to resume later.
                local_tasks_.push_back(node->self_handle);
            },
            opts_.batch_max_size);

        // A tick that already has handles staged must not park: step 4 is going
        // to resume them, and the buffer pool's reschedule hook can leave work
        // staged there without any I/O in flight to make the wait return early.
        // Sleeping through it would add a heartbeat of latency to every waiter.
        // Submitting still has to happen (step 3 can free SQ entries), it just
        // does not have to wait.
        const bool has_staged_work = !local_tasks_.empty();

        // 2. Submit, then park for at most the heartbeat interval. This one
        // call does both: flushing pushes the SQEs prepared by get_sqe() (under
        // DEFER_TASKRUN the deferred task work runs here too), and the wait
        // sleeps in the kernel when there is nothing to reap.
        //
        // The park is BOUNDED, and that is what makes this loop simple. A task
        // enqueued by another thread while we sleep is not a lost wakeup -- the
        // deadline brings us back and step 1 of the next tick finds it. There is
        // no wake channel to keep in sync with the worker's state, and therefore
        // no may_park() and no pre-sleep recheck of every work source.
        //
        // liburing returns the number of SQEs the kernel consumed, so this is
        // POSITIVE on success -- only negative is an error. -ETIME just means
        // the heartbeat expired with nothing to report, the normal idle path.
        __kernel_timespec heartbeat{};
        if (park_timeout == nullptr)
        {
            heartbeat.tv_sec = 0;
            heartbeat.tv_nsec = static_cast<long long>(heartbeat_us_) * 1000;
        }

        // liburing writes through cqe_ptr unconditionally, so it cannot be null
        // even though we harvest the CQ ourselves below.
        io_uring_cqe* cqe = nullptr;
        const int ret =
            (park_timeout != nullptr)
                ? io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, park_timeout, nullptr)
                : (has_staged_work
                       ? io_uring_submit_and_get_events(&ring_)
                       : io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &heartbeat, nullptr));

        // Submit paths return the number of SQEs consumed, positive on success.
        // The non-blocking flush has no -ETIME of its own, but accepting it here
        // keeps the one error check below valid for all three.
        if (ret < 0 && ret != -EINTR && ret != -ETIME)
        {
            KIO_LOG_ERROR("failed to submit and wait: {}", std::strerror(-ret));
        }

        // 3. Harvest the CQ. Every CQE is one of two kinds, told apart purely
        // by user_data: our own cancel-all, or a pointer into a live coroutine
        // frame (invariant I2). No registry, no lookup table.
        //
        // Capped at batch_max_size. Anything beyond the cap is LEFT IN THE RING
        // and picked up next tick -- not dropped, and not carried in a side
        // vector. The CQ tail is sampled once, like io_uring_for_each_cqe()
        // does, so completions arriving mid-walk are simply the next tick's
        // business; the kernel fills the slots we release as we advance.
        unsigned head = 0;
        unsigned harvested = 0;
        bool did_work = false;

        const __u32 cq_tail = io_uring_smp_load_acquire(ring_.cq.ktail);

        for (head = *ring_.cq.khead; harvested < opts_.batch_max_size && head != cq_tail; ++head, ++harvested)
        {
            cqe = &ring_.cq.cqes[io_uring_cqe_index(&ring_, head, ring_.cq.ring_mask)];

            if (io_uring_cqe_get_data64(cqe) == kCancelSentinel)
            {
                // res is the number of requests the cancel matched.
                //
                // Also not a resume: the operations it cancelled have their own
                // CQEs, which arrive separately and resume their own coroutines.
                KIO_LOG_INFO("Worker {} cancelled {} in-flight operation(s)", id_, cqe->res);
            }
            else
            {
                // The user case. user_data IS &ops_ inside a coroutine frame
                // that is suspended right now, so the write below cannot
                // outlive the storage.
                auto* op = reinterpret_cast<IoOps*>(io_uring_cqe_get_data64(cqe));
                op->res = cqe->res;

                // Queue the handle; do NOT resume it inline, because resuming
                // here would run user code before io_uring_cq_advance() released
                // these entries -- i.e. with the ring's head still pointing at
                // them.
                local_tasks_.push_back(op->h);
            }

            did_work = true;
        }

        // Hand the harvested slots back to the kernel in one go.
        if (harvested > 0)
        {
            io_uring_cq_advance(&ring_, harvested);
        }

        // Every SQE produces exactly one CQE; this is what outstanding_ops()
        // subtracts from the SQ tail to find work the kernel still owns. CQEs we
        // did not reach are still uncounted, so they correctly still count as
        // outstanding.
        completed_count_ += harvested;

        // 4. Resume. swap() first so local_tasks_ is genuinely empty for the
        // duration: resuming a coroutine can push new handles onto it (the
        // buffer pool's reschedule hook, a nested completion), and iterating a
        // vector that is being appended to is undefined.
        //
        // There is no per-tick cap on this loop. The old kMaxResumesPerTick
        // existed to bound how long one tick could run before the stop check;
        // the harvest above already bounds that, at batch_max_size.
        if (!local_tasks_.empty())
        {
            did_work = true;

            current_batch.swap(local_tasks_);

            for (const auto handle : current_batch)
            {
                handle.resume();
            }

            current_batch.clear();
        }

        // 5. Adaptive heartbeat. A tick that found work parks at the short
        // interval next time, so cross-thread posts stay responsive under load;
        // a tick that found nothing backs off towards max_idle_wait_us, so an
        // idle worker settles into one wakeup per long interval instead of
        // spinning at the short one.
        heartbeat_us_ = did_work ? opts_.heartbeat_interval_us
                                 : std::min(heartbeat_us_ * 2, opts_.max_idle_wait_us);
    }

    void IO::run_blocking(std::stop_token st) noexcept
    {
        // The ring is SINGLE_ISSUER, so ownership is claimed by whichever thread
        // actually drives the loop -- not by whichever thread constructed the IO.
        // IoContext builds every IO on the caller thread and then hands each one
        // to a jthread that calls run_blocking(), so capturing in the constructor
        // would name the wrong thread.
        owner_thread_ = std::this_thread::get_id();

        is_running_ = true;

        pin_to_cpu();

        activate();

        // Set the latch the admission gates read. Nothing else: the park is
        // bounded, so the loop notices this within one heartbeat interval on its
        // own and there is no wake to send.
        std::stop_callback latch_on_stop{
            st, [this] { stop_requested_.store(true, std::memory_order_release); }
        };
        while (!st.stop_requested())
        {
            tick();
        }

        is_running_ = false;

        KIO_LOG_INFO("Worker {} quiescing...", id_);
        if (buffer_pool_.has_value())
        {
            buffer_pool_->close();
        }

        // Drain remaining tasks so cancelled coroutines can resume and clean up
        drain_until_quiescent();
    }

    void IO::submit_cancel_all() noexcept
    {
        io_uring_sqe* sqe = get_sqe();
        if (sqe == nullptr)
        {
            // SQ is full: submit what we have and retry once. If it still
            // fails there is nothing we can cancel with -- the ring teardown
            // will reclaim the kernel side.
            KIO_LOG_ERROR("Worker {}: no SQE available to submit cancel-all", id_);
            return;
        }

        // IORING_ASYNC_CANCEL_ANY matches every in-flight request regardless of
        // user_data, so this needs no registry of outstanding operations. res
        // on the resulting CQE is the number of requests matched.
        io_uring_prep_cancel64(sqe, 0, IORING_ASYNC_CANCEL_ANY);
        io_uring_sqe_set_data64(sqe, kCancelSentinel);

        if (const int ret = io_uring_submit(&ring_); ret < 0)
        {
            KIO_LOG_ERROR("Worker {}: cancel-all submit failed: {}", id_, std::strerror(-ret));
        }
    }

    namespace
    {
        __kernel_timespec timespec_until(const std::chrono::steady_clock::time_point deadline) noexcept
        {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = deadline > now ? deadline - now : std::chrono::steady_clock::duration::zero();
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(remaining);
            const auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - secs);

            __kernel_timespec ts{};
            ts.tv_sec = secs.count();
            ts.tv_nsec = static_cast<long long>(nsecs.count());
            return ts;
        }
    } // namespace

    bool IO::has_pending_work() const noexcept
    {
        return outstanding_ops() > 0 || !local_tasks_.empty() || !queue_.empty() || io_uring_cq_ready(&ring_) > 0;
    }

    void IO::drain_until_quiescent() noexcept
    {
        // Deliberately NOT using an IOSQE_IO_DRAIN barrier here. Measured on
        // this kernel: a pending drain request serialises the submission
        // pipeline, so the cancel SQE submitted behind it never executes and
        // the escalation below becomes impossible. The bounded park plus the
        // outstanding_ops() check is what actually detects a stuck operation.
        //
        // (Confirmed twice, independently. The first probe was confounded by an
        // incorrect assumption about what liburing's cancel helper returns, so
        // it was redone with the return value read directly.)
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(opts_.shutdown_grace_ms);

        // WHY THIS TERMINATES, in both phases:
        //   * phase 1 is bounded by the wall clock -- it cannot outlast the
        //     deadline regardless of what the kernel is doing.
        //   * phase 2 is bounded by its own hard_deadline, for the same reason.
        // There is no unbounded loop anywhere in this function, which is the
        // whole design constraint: teardown must never be able to wedge a
        // worker forever on a single stuck read.
        //
        // Note both phases rely on the admission gate being closed. If tasks
        // could still be accepted they could still submit operations, and the
        // quiescence check would keep failing. See schedule() and
        // IoAwaiter::await_suspend in io.h.

        // Phase 1 -- graceful. Resume anything runnable; when there is nothing
        // runnable but the kernel still owns work, park (bounded) waiting for
        // it to complete on its own.
        while (has_pending_work() && std::chrono::steady_clock::now() < deadline)
        {
            auto ts = timespec_until(deadline);
            tick(&ts);
        }

        if (outstanding_ops() > 0)
        {
            KIO_LOG_WARN("Worker {}: {} operation(s) unfinished after {}ms grace; cancelling", id_,
                         outstanding_ops(), opts_.shutdown_grace_ms);
            submit_cancel_all();

            // Phase 2 -- drain the cancellations. Cancelled operations complete
            // with -ECANCELED, which resumes their coroutines so they unwind
            // through the normal error path instead of being stranded. Parks are
            // bounded here too so the wait neither spins nor outlives the deadline.
            const auto hard_deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(opts_.shutdown_grace_ms);
            while (has_pending_work())
            {
                if (std::chrono::steady_clock::now() >= hard_deadline)
                {
                    KIO_LOG_ERROR("Worker {}: {} operation(s) still outstanding after cancel; "
                                  "abandoning drain",
                                  id_, outstanding_ops());
                    break;
                }
                auto ts = timespec_until(hard_deadline);
                tick(&ts);
            }
        }

        KIO_LOG_INFO("Worker {} quiesced", id_);
    }

    void IO::note_sq_full() noexcept
    {
        if ((sq_full_events_++ & 0xFFF) == 0)
        {
            KIO_LOG_WARN("Worker {}: SQ ring full; returning EAGAIN to apply backpressure ({} event(s) so far)",
                         id_, sq_full_events_);
        }
    }

    io_uring_sqe* IO::get_sqe() noexcept
    {
        // IORING_SETUP_SINGLE_ISSUER: only the owner thread may produce SQEs.
        assert(std::this_thread::get_id() == owner_thread_ &&
            "IO::get_sqe() called from a non-owner thread; the ring is SINGLE_ISSUER "
            "(check for a missing co_await io.schedule_on(...))");

        // The assert is the real enforcement and it is debug-only. In release
        // builds a stray non-owner call would not be caught here -- which is
        // exactly why the transfer is a documented one-way door rather than
        // something this function tries to detect and recover from. See I1.

        // A full SQ gets exactly one eager submit, not a spin loop: the kernel
        // may still be holding the SQEs we just flushed, so retrying
        // immediately would busy-wait on the kernel instead of on our own
        // progress. If this still returns null the ring is genuinely
        // saturated and the caller reports EAGAIN as backpressure.
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            // SQ is full. Try one submit to clear space.
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
        }
        return sqe;
    }

    void IO::pin_to_cpu() const
    {
        // TODO: use modulo to map when id_ >= opts_.worker_cpu_affinity.size()
        if (empty(opts_.worker_cpu_affinity) || id_ >= opts_.worker_cpu_affinity.size())
        {
            return;
        }

        const int physical_core_id = opts_.worker_cpu_affinity[id_];
        if (physical_core_id < 0 || physical_core_id >= CPU_SETSIZE)
        {
            KIO_LOG_WARN("Worker {}: ignoring invalid CPU id {} in worker_cpu_affinity", id_, physical_core_id);
            return;
        }

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(static_cast<size_t>(physical_core_id), &cpuset);

        if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
        {
            KIO_LOG_INFO("Warning: Failed to pin to physical CPU {}: {}", physical_core_id,
                         std::generic_category().message(rc));
        }
    }

    IO::~IO()
    {
        // Contract: tasks still queued here are abandoned, not destroyed. A queued
        // handle may be a child frame owned by its parent's frame, so destroying
        // it blindly could free it twice. This only happens when the IO is torn
        // down without being drained (run_blocking() drains before returning).
        if (const auto abandoned = local_tasks_.size() + queue_.drain([](detail::TaskPromiseBase*)
            {
            });
            abandoned > 0)
        {
            KIO_LOG_WARN("Worker {}: destroyed with {} task(s) never resumed; their frames are leaked", id_,
                         abandoned);
        }

        // Registered buffers need no explicit unregister: io_uring_queue_exit()
        // drops them. Unregistering here would also fail with -EEXIST on a
        // SINGLE_ISSUER ring, since the destroying thread is rarely the issuer.
        if (ring_.ring_fd >= 0)
        {
            io_uring_queue_exit(&ring_);
            ring_.ring_fd = -1;
        }
    }
} // namespace kio
