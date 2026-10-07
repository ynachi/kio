#include "uring/core/io.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <future>
#include <iterator>

#include <unistd.h>

#include <sys/eventfd.h>

#include "uring/core/awaiter.hpp"

namespace kio
{
    IO::IO(const size_t id, const IO* leader, const IoOptions& opts, const std::optional<BufferPoolConfig> pool_cfg)
        : opts_(opts), id_(id)
    {
        opts_.batch_max_size = std::max<std::size_t>(1, opts_.batch_max_size);
        local_tasks_.reserve(opts_.entries);
        current_batch.reserve(kMaxResumesPerTick);

        if (pool_cfg.has_value())
        {
            buffer_pool_.emplace(pool_cfg->slot_size, pool_cfg->slots);
            buffer_pool_->set_reschedule_hook(
                this,
                [](void* ctx, std::coroutine_handle<> h) noexcept
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
        : is_activated_(other.is_activated_),
          is_running_(other.is_running_),
          stop_requested_(other.stop_requested_.load(std::memory_order_seq_cst)),
          opts_(other.opts_),
          id_(other.id_),
          buffer_pool_(std::move(other.buffer_pool_)),
          registered_iovecs_(std::move(other.registered_iovecs_))
    {
        // Safety check, we SHOULD not move a running IO. An activated one is not
        // inert either: its wake read is still in flight against other.wake_value_,
        // and completed_count_/armed_wake_reads_ would not travel with the ring.
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

        // Transfer FDs
        wake_fd_ = std::exchange(other.wake_fd_, -1);

        // Transfer state
        wake_value_ = other.wake_value_;
    }

    void IO::init(const int wq_fd)
    {
        io_uring_params params{};

        // disable the ring first
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

        wake_fd_ = eventfd(0, EFD_CLOEXEC);
        if (wake_fd_ < 0)
        {
            io_uring_queue_exit(&ring_);
            throw std::runtime_error("eventfd failed");
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
                ::close(wake_fd_);
                wake_fd_ = -1;
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

        // we need to submit a first read, and this is a good place
        arm_wake_read();
        is_activated_ = true;
    }

    void IO::submit_or_wait_for(__kernel_timespec* park_timeout)
    {
        // Non-blocking flush: push pending SQEs and, under DEFER_TASKRUN, run the
        // deferred task work that actually posts completions.
        const auto flush = [this]
        {
            const int ret = (opts_.flags & IORING_SETUP_DEFER_TASKRUN) ? io_uring_submit_and_get_events(&ring_)
                                                                        : io_uring_submit(&ring_);
            if (ret < 0 && ret != -EINTR)
            {
                KIO_LOG_ERROR("failed to submit: {}", std::strerror(-ret));
            }
        };

        // Only block if we have no local work to do.
        if (io_uring_cq_ready(&ring_) > 0 || !local_tasks_.empty() || !queue_.empty())
        {
            flush();
            return;
        }

        // Thundering herd mitigation: we recheck every source of work before
        // parking. Because wake() writes the eventfd unconditionally, a producer
        // that raced us either already left its token in the eventfd counter
        // (level triggered, so the park returns at once) or writes it after we
        // park.
        //
        // Two cases must not park without a deadline:
        //  - stop_requested_: the loop's own stop test has already passed by the
        //    time we get here, so a stop landing in that window would be slept
        //    through.
        //  - no wake read armed: nothing could interrupt the park.
        // Both flush instead; the loop comes back around and re-evaluates.
        //
        // A bounded park (teardown) is always safe: it cannot sleep forever, and it
        // must actually park, or the drain would spin without ever entering the
        // kernel to reap completions.
        const bool may_park = park_timeout != nullptr ||
                              (!stop_requested_.load(std::memory_order_seq_cst) && armed_wake_reads_ > 0);
        if (!may_park)
        {
            flush();
            return;
        }

        // Announce the park, then recheck every source of work. Producers that
        // enqueued before this point are seen by the recheck; producers after it
        // see sleeping_ and write the eventfd (see IO::post()).
        sleeping_.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (io_uring_cq_ready(&ring_) > 0 || !local_tasks_.empty() || !queue_.empty())
        {
            sleeping_.store(false, std::memory_order_relaxed);
            flush();
            return;
        }

        // NOTE: liburing returns the count of SQEs the kernel consumed,
        // so this is POSITIVE on success -- only negative is an error.
        int ret = 0;
        if (park_timeout != nullptr)
        {
            // liburing writes through cqe_ptr unconditionally, so this
            // cannot be null even though we only care about the wait.
            io_uring_cqe* cqe = nullptr;
            ret = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, park_timeout, nullptr);
        }
        else
        {
            ret = io_uring_submit_and_wait(&ring_, 1);
        }

        sleeping_.store(false, std::memory_order_relaxed);

        // -ETIME is the bounded park expiring, which is the expected outcome.
        if (ret < 0 && ret != -EINTR && ret != -ETIME)
        {
            KIO_LOG_ERROR("failed to submit and wait: {}", std::strerror(-ret));
        }
    }

    void IO::tick(__kernel_timespec* park_timeout) noexcept
    {
        // A re-arm skipped because the SQ was full is only retried here: with no
        // wake read in flight nothing would ever re-arm it, and post()/stop could
        // no longer interrupt a park.
        if (armed_wake_reads_ == 0 && is_activated_)
        {
            arm_wake_read();
        }

        // Drain the cross-thread MPSC queue first.
        queue_.drain(
            [this](detail::TaskPromiseBase* node)
            {
                // We retrieve the safe handle to resume later.
                local_tasks_.push_back(node->self_handle);
            },
            opts_.batch_max_size);

        // wait for completion if needed
        submit_or_wait_for(park_timeout);

        // Batch process CQEs into the local queue
        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            auto user_data = io_uring_cqe_get_data64(cqe);

            if (user_data == kWakeupSentinel)
            {
                if (armed_wake_reads_ > 0)
                {
                    --armed_wake_reads_;
                }
                arm_wake_read();
            }
            else if (user_data == kCancelSentinel)
            {
                // res is the number of requests the cancel matched.
                KIO_LOG_INFO("Worker {} cancelled {} in-flight operation(s)", id_, cqe->res);
            }
            else
            {
                auto* op = reinterpret_cast<IoOps*>(user_data);
                op->res = cqe->res;

                local_tasks_.push_back(op->h);
            }
        }

        // Every SQE produces exactly one CQE; this is what outstanding_ops()
        // subtracts from the SQ tail to find work the kernel still owns.
        completed_count_ += count;

        if (count > 0)
        {
            // Free all the kernel slots at once
            io_uring_cq_advance(&ring_, count);
        }

        // Execute completions, capped per tick. Resuming an unbounded batch let
        // one large completion burst monopolise the worker: the stop check only
        // runs between ticks. The remainder carries over instead of being
        // dropped, so nothing is lost.
        if (!local_tasks_.empty())
        {
            current_batch.swap(local_tasks_);

            const size_t resume_count = std::min(current_batch.size(), kMaxResumesPerTick);
            for (size_t i = 0; i < resume_count; ++i)
            {
                current_batch[i].resume();
            }

            if (resume_count < current_batch.size())
            {
                // The resumes above consumed the FRONT of the batch, so the
                // un-started tasks are current_batch[resume_count .. end).
                // Tasks resumed above may have rescheduled themselves into
                // local_tasks_, so append the leftovers after those.
                const auto carried = current_batch.begin() + static_cast<std::ptrdiff_t>(resume_count);
                local_tasks_.insert(
                    local_tasks_.end(),
                    std::make_move_iterator(carried),
                    std::make_move_iterator(current_batch.end()));
            }

            current_batch.clear();
        }
    }

    void IO::arm_wake_read() noexcept
    {
        io_uring_sqe* sqe = get_sqe();
        if (sqe == nullptr)
        {
            // SQ is full. Do not dereference null: skip the re-arm. The eventfd
            // is level-triggered and keeps its token, so the next completion that
            // re-arms will pick it up -- the wake is delayed, never lost.
            KIO_LOG_WARN("SQ ring full, deferring wake-read re-arm");
            return;
        }

        io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
        io_uring_sqe_set_data64(sqe, kWakeupSentinel);
        ++armed_wake_reads_;
        // No io_uring_submit() here: the next submit_or_wait_for() flushes this SQE,
        // and it always runs before the worker can park. An inline submit cost one
        // extra enter per wake.
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

        // Publish the flag before waking so the pre-sleep recheck cannot miss it.
        std::stop_callback wake_on_stop{st, [this] {
            stop_requested_.store(true, std::memory_order_seq_cst);
            wake();
        }};
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
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(opts_.shutdown_grace_ms);

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

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            // SQ is full. Try one submit to clear space.
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
        }
        return sqe;
    }

    void IO::wake() const noexcept
    {
        // Write unconditionally. Gating this on a "worker is sleeping" flag lost
        // any wake that arrived before the worker parked -- notably a stop
        // request, which then wedged the worker in io_uring_submit_and_wait()
        // forever. The eventfd is level-triggered and accumulates, so an
        // unconsumed write only costs one extra loop iteration later.
        constexpr uint64_t one = 1;
        for (;;)
        {
            const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
            if (n == sizeof(one) || (n == -1 && errno == EAGAIN))
            {
                return;
            }
            if (n == -1 && errno == EINTR)
            {
                continue;
            }
            KIO_LOG_WARN("failed to wake io context with eventfd: {}", std::strerror(errno));
            return;
        }
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
        CPU_SET(physical_core_id, &cpuset);

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
        if (const auto abandoned = local_tasks_.size() + queue_.drain([](detail::TaskPromiseBase*) {});
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

        if (wake_fd_ >= 0)
        {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
    }
} // namespace kio
