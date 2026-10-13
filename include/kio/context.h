#pragma once

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "detail/awaitable.h"
#include "detail/trivial_ring_buffer.h"

#include <algorithm>
#include <coroutine>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <liburing.h>
#include <unistd.h>

#include "buffer_pool.hpp"

namespace kio
{
template <class T>
class Task;

class Baton;
class Semaphore;
}  // namespace kio

namespace kio::io
{
struct IoAwaitableBase;

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
    template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep>
    friend class IoAwaitable;

    friend void cancel(Context&, IoAwaitableBase&) noexcept;

    friend class kio::Baton;      // NOLINT > the linter would say the namespace is not needed, don't listen
    friend class kio::Semaphore;  // NOLINT > the linter would say the namespace is not needed, don't listen

    int ring_fd() const noexcept { return ring_.ring_fd; }

    void init(const int wq_fd, const std::optional<BufferPoolConfig>& pool_cfg)
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

public:
    explicit Context(const size_t id, const Context* leader = nullptr, const IoOptions& opts = {},
                     const std::optional<BufferPoolConfig>& pool_cfg = std::nullopt)
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
    void check_loop_thread() const noexcept {}
#endif

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
        // Bind/debug-CHECK the loop-thread contract: everything below
        // (and every post/note_park from primitives) assumes one driver.
        check_loop_thread();

        constexpr std::uint64_t kChunk = 256;
        std::coroutine_handle<> chunk[kChunk];

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

            if (ready_list_.empty() && inflight_ == 0)
            {
                CHECK((!buffer_pool_ || buffer_pool_->waiting() == 0) && external_parks_ == 0)
                    << "parked waiter with no one left to wake it "
                       "(pool leases leaked? baton never posted? semaphore never released?)";

                io_uring_submit(&ring_);

                return;
            }

            // Phase 2: submit, and block for a CQE only when nothing is runnable.
            const int r =
                ready_list_.empty() ? io_uring_submit_and_wait(&ring_, 1) : io_uring_submit_and_get_events(&ring_);
            if (r < 0 && r != -EINTR && r != -EBUSY)
            {
                LOG(FATAL) << "fatal, io submit failed: " << r;
                std::abort();
            }
            // reap. Only queues handles into the ready list, never resumes.
            reap();
        }
    }

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

    io_uring_sqe* get_sqe(const unsigned reserve = 1) noexcept
    {
        if (io_uring_sq_space_left(&ring_) < reserve)
        {
            int r = 0;
            while ((r = io_uring_submit(&ring_)) == -EINTR)
            {
            }
            if (r == -EBUSY)
            {
                reap();
                while ((r = io_uring_submit(&ring_)) == -EINTR)
                {
                }
            }
            if (r < 0)
            {
                LOG(FATAL) << "fatal, io submit failed: " << r;
                std::abort();
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

    void reap() noexcept
    {
        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned n = 0;
        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            ++n;
            const std::uint64_t ud = io_uring_cqe_get_data64(cqe);
            if (ud & IGNORE_TAG)
            {
                continue;
            }
            auto* op = reinterpret_cast<IoAwaitableBase*>(ud);
            CHECK(op->lifecycle() == IoAwaitableState::Inflight) << "CQE for an op that is not in flight";
            op->res = cqe->res;
            op->set_lifecycle(IoAwaitableState::Done);
            op_finished();
            post(op->handle);
        }
        io_uring_cq_advance(&ring_, n);
    }

    void cancel(IoAwaitableBase& op) noexcept
    {
        // One cancel SQE per op: a repeat would submit a second
        // IGNORE_TAG SQE for nothing, and re-flagging is meaningless.
        if (op.lifecycle() != IoAwaitableState::Inflight || op.cancel_requested())
        {
            return;
        }

        op.set_cancel_requested();
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, &op, 0);
        io_uring_sqe_set_data64(sqe, IGNORE_TAG);
    }

    void activate() noexcept
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
                           << (rc == -ENOMEM ? "; check `ulimit -l` (RLIMIT_MEMLOCK) or LimitMEMLOCK= in the unit file"
                                             : "");
            }
        }

        is_activated_ = true;
    }
};

template <std::invocable<io_uring_sqe*, __kernel_timespec*> Prep>
void IoAwaitable<Prep>::await_suspend(const std::coroutine_handle<> h) noexcept
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
}  // namespace kio::io
