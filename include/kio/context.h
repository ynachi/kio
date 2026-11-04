#pragma once

#include "absl/log/check.h"
#include "detail/awaitable.h"
#include "detail/trivial_ring_buffer.h"

#include <coroutine>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <liburing.h>
#include <stop_token>
#include <unistd.h>

#include "buffer_pool.hpp"
#include "fd.hpp"

namespace kio
{
    template <class T>
    class Task;

    class Baton;
    class Semaphore;
} // namespace kio

namespace kio::io
{
    inline constexpr unsigned kUringDefaultFlag =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    static constexpr int kDefaultAcceptFlags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    // Prefetch distance: how many resumes ahead to warm a coroutine
    // frame's header line.
    inline constexpr std::uint64_t kResumePrefetchDist = 2;

    struct IoOptions
    {
        std::uint32_t entries = 4096;
        unsigned flags = kUringDefaultFlag;

        bool share_work_queues = true;

        // Max coroutine resumes per run-loop epoch
        std::size_t batch_max_size = 128;

        std::uint32_t sq_thread_idle_ms = 2000;
        // -1 means don't pin to a specific CPU
        int sq_thread_cpu = -1;
    };

    class Context
    {
        template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep, typename T>
        friend class detail::IoAwaitable;

        friend void cancel(Context&, detail::IoAwaitableBase&) noexcept;

        friend class kio::Baton; // NOLINT > the linter would say the namespace is not needed, don't listen
        friend class kio::Semaphore; // NOLINT > the linter would say the namespace is not needed, don't listen

        int ring_fd() const noexcept { return ring_.ring_fd; }

        void init(int wq_fd, const std::optional<BufferPoolConfig>& pool_cfg);

    public:
        explicit Context(size_t id, const Context* leader = nullptr, const IoOptions& opts = {},
                         const std::optional<BufferPoolConfig>& pool_cfg = std::nullopt);

        ~Context()
        {
            CHECK(inflight_ == 0) << "Context destroyed with operations still in flight";
            CHECK(ready_list_.empty()) << "Context destroyed with unstarted work (spawn() without run())";
            io_uring_queue_exit(&ring_);
        }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        // Should be created where it will live as the SINGLE_ISSUER flag would
        // dislike it to be moved.
        Context(Context&&) = delete;
        Context& operator=(Context&&) = delete;

        /// @brief This Context's constructor id (stable, >= 0).
        [[nodiscard]] size_t id() const noexcept { return id_; }

        [[nodiscard]] std::runtime_error make_context_error(const std::string_view what, const int rc) const
        {
            return std::runtime_error(std::format("{} failed: {} (flags: {:#x}, entries: {})", what, std::strerror(-rc),
                                                  opts_.flags, opts_.entries));
        }

#ifndef NDEBUG
        // Debug-mode thread-identity CHECK for the loop-thread contract
        // (sharding.md rule 5). The run loop and every post/park mutate plain
        // members; a second thread doing either is UB that only sanitizer
        // builds would notice. Cheap and fatal in debug: the first
        // cross-thread post aborts at the bug site.
        //
        // Enforcement starts exactly when the loop does: run() activates
        // before it binds, so everything up to the first run() -- including
        // spawn-before-run from any thread -- is the supported setup pattern
        // the R_DISABLED note in init() describes.
        void check_loop_thread() const noexcept
        {
            if (!is_activated_)
            {
                return;
            }
            CHECK(owner_tid_ == 0 || owner_tid_ == ::gettid())
            << "kio loop-thread contract violated: Context touched from thread " << ::gettid() << ", owned by thread "
            << owner_tid_;
            owner_tid_ = ::gettid();
        }
#else
        void check_loop_thread() const noexcept
        {
        }
#endif

        template <class T>
        void spawn(Task<T> task) noexcept;

        /// @brief Runs until quiesced: no ready coroutine and no in-flight op.
        /// The token is read at exactly ONE point -- the loop's arming check.
        /// A stop request arms a force-drain: cancel_all() plus fail-fast
        /// staging (ops awaited after the latch complete inline with
        /// -ECANCELED, never staged as SQEs), and run() still returns only
        /// when quiesced -- it never abandons work. Graceful shutdown is the
        /// application's choreography: finish admitted work and let quiesce
        /// end the session, or request_stop() as the escalation
        /// (design-notes/shutdown.md). A default token degenerates to the
        /// pure quiesce-only loop (which blocks unwakeably while an op is
        /// pending -- sharding.md §6).
        void run(const std::stop_token& st = {}) noexcept;

        /// @brief Query the registered buffer pool, if configured.
        [[nodiscard]] const std::optional<RegisteredBufferPool>& buffer_pool() const noexcept { return buffer_pool_; }

        /// @brief Try to take a slot without parking (empty lease if none).
        [[nodiscard]] BufferLease try_acquire_buffer() noexcept
        {
            return buffer_pool_ ? buffer_pool_->try_acquire() : BufferLease{};
        }

        /// @brief Asynchronous FIFO acquisition of a pooled slot.
        [[nodiscard]] RegisteredBufferPool::AcquireAwaiter acquire_buffer() noexcept
        {
            CHECK(buffer_pool_.has_value()) << "acquire_buffer: no buffer pool configured";
            return buffer_pool_->acquire();
        }

        // Called on teardown -- or by run() itself the moment the stop
        // token fires (the loop thread is the only legal submitter).
        void cancel_all() noexcept
        {
            // Latched so ops completed by this cancel report cancellation
            // instead of a bogus ETIMEDOUT (their await_resume cannot tell a
            // teardown cancel from a fired linked timeout otherwise).
            // Cleared at the top of the next run().
            canceling_all_ = true;
            io_uring_sqe* s = get_sqe();
            io_uring_prep_cancel64(s, 0, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY);
            io_uring_sqe_set_data64(s, detail::kIgnoreTag);
        }

    private:
        detail::ReadyRing ready_list_{};
        io_uring ring_{};
        uint64_t inflight_{};
        std::size_t external_parks_ = 0;
        bool is_activated_{false};
        // Set by cancel_all(), read by IoAwaitable::await_resume so ops
        // canceled by a teardown report -ECANCELED instead of a fake
        // ETIMEDOUT.
        bool canceling_all_{false};

        std::optional<RegisteredBufferPool> buffer_pool_{std::nullopt};
        std::vector<iovec> registered_iovecs_{};
        IoOptions opts_;
        size_t id_;
        // Loop-thread identity for the debug-mode contract CHECK (0 = unset).
        // Mutable: check_loop_thread() reads const and stamps the binding on
        // first call. Debug-only traffic; lives cold so release builds waste
        // no cache line.
        mutable pid_t owner_tid_ = 0;

        // wake up mechanism
        // For now, the eventfd is used to wake up the thread, not for cross-thread work stealing
        UniqueFd wake_fd_{};
        std::uint64_t wake_buf_ = 0;
        bool wake_armed_ = false;
        void arm_wake() noexcept;

        io_uring_sqe* get_sqe(unsigned reserve = 1) noexcept;
        void submit_or_die() noexcept;

        void op_started() noexcept { ++inflight_; }

        void op_finished() noexcept { --inflight_; }

        void post(const std::coroutine_handle<> handle) noexcept
        {
            check_loop_thread();
            CHECK(handle.address() != nullptr) << "post of a null coroutine handle";
            // Note: ReadyRing::push allocates only when it grows; steady
            // state never allocates (the ring drains to empty at every phase
            // boundary, so capacity stays warm). A grow that throws
            // std::bad_alloc inside this noexcept context calls
            // std::terminate(), which matches this library's error handling
            // philosophy.
            // TODO: handle std::bad_alloc. I am also tempted to not allow growth at all.
            // Check what is feasible
            ready_list_.push(handle);
        }

        // Coroutines parked on non-ring primitives (Baton/Semaphore and
        // any future parking awaiter): they are invisible to inflight_ and
        // ready_list_, so they must teach run() about themselves. Parked
        // increments in await_suspend; the woken await_resume consumes it;
        // between wake and resume the handle sits in ready_list_
        void note_park() noexcept { ++external_parks_; }

        void note_unpark() noexcept { --external_parks_; }

        void reap() noexcept;

        void cancel(detail::IoAwaitableBase& op) noexcept;

        void activate() noexcept;
    };
} // namespace kio::io

namespace kio::detail
{
    // 1. Added `typename T` and `<Prep, T>`
    template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep, typename T>
    bool IoAwaitable<Prep, T>::await_suspend(const std::coroutine_handle<> h) noexcept
    {
        // One await per op: state leaves Created exactly once, and a second
        // co_await (state Inflight or Done) would strand the first waiter or
        // double-consume the result.
        CHECK(lifecycle() == IoAwaitableState::Created) << "IoAwaitable awaited twice";
        if (io_.canceling_all_)
        {
            // Force-drain in progress: refuse to stage. Returning false
            // resumes the coroutine immediately (it was never suspended),
            // await_resume classifies -ECANCELED inline, and inflight_ is
            // monotone during the drain -- quiesce is a termination proof,
            // not a hope. The task's only sanctioned exit is to observe the
            // cancellation and stop re-awaiting; a blind retry loop spins
            // in Phase 1 and run() never returns (documented contract).
            res = -ECANCELED;
            set_lifecycle(IoAwaitableState::Done);
            return false;
        }
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
            io_uring_sqe_set_data64(t, kIgnoreTag);
        }

        io_.op_started();
        return true;
    }

    // 2. Added `typename T` and `<Prep, T>`
    // 3. Changed `Result<int>` to `Result<T>`
    template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep, typename T>
    Result<T> IoAwaitable<Prep, T>::await_resume() const noexcept
    {
        if (res >= 0)
        {
            return res; // Implicitly converts int to T if T is int, which is the default
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
} // namespace kio::detail
