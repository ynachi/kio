#pragma once

#include <cassert>
#include <coroutine>
#include <cstdlib>
#include <cstring>
#include <liburing.h>
#include <print>
#include <queue>

#include "buffer_pool.hpp"
#include "detail/awaitable.h"

namespace kio
{
    template <class T>
    class Task_v2;
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

            // TODO: register buffer

            KIO_LOG_INFO("Initialized IO context with {} entries (SQPOLL: {})", opts_.entries,
                         (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
        }

    public:
        explicit Context(const size_t id, const Context* leader = nullptr,
                         const IoOptions& opts = {}) : opts_(opts), id_(id)
        {
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
            assert(inflight_ == 0 && "urco: IO destroyed with operations still in flight");
            io_uring_queue_exit(&ring_);
        }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        // TODO: define the move ctor
        Context& operator=(Context&&) = delete;

        template <class T>
        void spawn(Task_v2<T> task) noexcept;

        // Runs until there is no ready coroutine and no in-flight op.
        void run() noexcept
        {
            activate();
            
            for (;;)
            {
                // Phase 1: resume everything that is ready.
                while (!ready_list_.empty())
                {
                    std::coroutine_handle<> h = ready_list_.front();
                    ready_list_.pop();
                    h.resume(); // Coroutine resumes and may queue more work
                }
                if (inflight_ == 0)
                {
                    io_uring_submit(&ring_); // flush stray SQEs
                    return;
                }
                // Phase 2: one syscall submits everything queued and waits for a CQE.
                int r = io_uring_submit_and_wait(&ring_, 1);
                if (r < 0 && r != -EINTR && r != -EBUSY)
                {
                    std::fprintf(stderr, "urco: submit_and_wait failed: %d\n", r);
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

    private:
        std::queue<std::coroutine_handle<>> ready_list_{}; // Classic data structure
        std::optional<RegisteredBufferPool> buffer_pool_{std::nullopt};
        io_uring ring_{};
        unsigned setup_flags_ = 0;
        std::vector<iovec> registered_iovecs_{};
        uint64_t inflight_{};
        IoOptions opts_;
        size_t id_;
        bool is_activated_{false};

        //
        // NEW METHODS, v2 migration
        //
        io_uring_sqe* get_sqe(const unsigned reserve = 1) noexcept
        {
            if (io_uring_sq_space_left(&ring_) < reserve)
            {
                io_uring_submit(&ring_);
            }
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);

            if (sqe == nullptr)
            {
                // queue full after submission is clearly a bug somewhere. Abort
                std::println(stderr, "urco: submission queue full");
                std::abort();
            }

            return sqe;
        }

        void op_started() noexcept { ++inflight_; }

        void op_finished() noexcept { --inflight_; }

        // Queue a coroutine handle to be resumed by the run loop.
        // Loop-thread only. A node must not be queued twice.
        void post(const std::coroutine_handle<> handle) noexcept
        {
            // Note: std::queue::push can theoretically throw std::bad_alloc.
            // If it does, because we are in a noexcept context, it will call std::terminate()
            // which matches this library's error handling philosophy.
            ready_list_.push(handle);
        }

        void reap() noexcept
        {
            io_uring_cqe* cqe;
            unsigned head, n = 0;
            io_uring_for_each_cqe(&ring_, head, cqe)
            {
                ++n;
                const std::uint64_t ud = io_uring_cqe_get_data64(cqe);
                if (ud & IGNORE_TAG) continue; // link-timeout / cancel CQE
                auto* op = reinterpret_cast<IoAwaitableBase*>(ud);
                op->res = cqe->res;
                // TODO: I feel like done is not right here
                if (op->state != IoAwaitableState::Canceled)
                {
                    op->switch_state(IoAwaitableState::Done);
                }
                op_finished();
                post(op->handle);
                // `op` may be destroyed as soon as its coroutine runs: don't touch it again.
            }
            io_uring_cq_advance(&ring_, n);
        }

        void cancel(IoAwaitableBase& op) noexcept
        {
            if (op.state != IoAwaitableState::Inflight)
                return;

            io_uring_sqe* sqe = get_sqe();
            op.switch_state(IoAwaitableState::Canceled);
            io_uring_prep_cancel(sqe, &op, 0);
            io_uring_sqe_set_data64(sqe, IGNORE_TAG);
        }

        void activate() noexcept
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
    };

    template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep>
    void IoAwaitable<Prep>::await_suspend(
        const std::coroutine_handle<> h) noexcept
    {
        set_handle(h);
        switch_state(IoAwaitableState::Inflight);
        io_uring_sqe* sqe = io_.get_sqe(link_ts_ ? 2 : 1);
        prep_(sqe, &scratch_);
        io_uring_sqe_set_data(sqe, static_cast<IoAwaitableBase*>(this));
        if (link_ts_)
        {
            sqe->flags |= IOSQE_IO_LINK;
            io_uring_sqe* t = io_.get_sqe(1);
            io_uring_prep_link_timeout(t, &*link_ts_, 0);
            io_uring_sqe_set_data64(t, IGNORE_TAG);
        }

        io_.op_started();
    }
} // namespace kio::io
