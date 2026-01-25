#pragma once

/**
 * aio.hpp - Minimal async I/O library built on io_uring and C++23 coroutines
 *
 * Design:
 * - Thread-per-core model: one io_context per thread, no cross-thread submission
 * - Coroutine handle stored in user_data for direct resume
 * - Intrusive tracking of pending operations for safe cancellation/shutdown
 * - Caller owns buffers; library does not allocate
 *
 * Safety:
 * Operation lifetimes are tracked via an intrusive linked list. If a coroutine
 * frame is destroyed while an I/O operation is still pending (kernel hasn't
 * completed it), the program will terminate rather than risk silent memory
 * corruption. This is detection, not prevention.
 *
 * !! IMPORTANT !!
 * Operations are embedded in coroutine frames. If you destroy a task while its
 * coroutine is suspended on I/O, you WILL hit std::terminate(). This is
 * intentional — the alternative is silent memory corruption when the kernel
 * writes to freed memory.
 *
 * To safely cancel work:
 * 1. Use timeouts (.with_timeout()) so operations eventually complete
 * 2. Let io_context::cancel_all_pending() drain on destruction
 * 3. Don't destroy tasks with pending I/O — wait for them to complete
 *
 * A future version may use a slab allocator to fully prevent UAF, but that
 * adds complexity inappropriate for this minimal implementation.
 *
 * Requirements:
 * - Linux kernel >= 6.0
 * - liburing
 * - C++23
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <csignal>
#include <exception>
#include <expected>
#include <latch>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <liburing.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/signalfd.h>

#include "aio/operation_base.hpp"
#include "aio/pipe_pool.hpp"
#include "aio/result.hpp"
#include "aio/task.hpp"

namespace aio
{

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

namespace detail
{
/// Reserved user_data value for internal eventfd wake
constexpr uint64_t WAKE_TAG = 1;
}  // namespace detail

// -----------------------------------------------------------------------------
// IoContext - The Event Loop
// -----------------------------------------------------------------------------

class IoContext
{
#ifndef NDEBUG
    std::thread::id owner_thread_ = std::this_thread::get_id();
#endif

    void AssertOwnerThread() const
    {
#ifndef NDEBUG
        if (std::this_thread::get_id() != owner_thread_)
        {
            // Could also use assert() but terminate is more visible
            std::fprintf(stderr, "IoContext accessed from wrong thread!\n");
            std::terminate();
        }
#endif
    }

public:
    // user can pass their own io uring flag.
    // Note: using single issuer must ensure single issuer constraints are met.
    explicit IoContext(const unsigned entries = 256)
    {
        io_uring_params params{};

        params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

        if (const int ret = io_uring_queue_init_params(entries, &ring_, &params); ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params");
        }

        // Reduce allocations in the hot path.
        ready_.reserve(entries);
        ext_done_.reserve(entries);

        // Initialize internal wake eventfd
        wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake_fd_ < 0)
        {
            io_uring_queue_exit(&ring_);
            throw std::system_error(errno, std::system_category(), "eventfd");
        }

        // Submit the initial read on the wake_fd
        SubmitWakeRead();
        io_uring_submit(&ring_);

        // signal that we are up
        ready_latch_.count_down();
    }

    ~IoContext() noexcept
    {
        CancelAllPending();
        if (wake_fd_ >= 0)
        {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
        io_uring_queue_exit(&ring_);
    }

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;

    // -------------------------------------------------------------------------
    // Thread-Safe Signaling
    // -------------------------------------------------------------------------

    /**
     * Wakes up the event loop from any thread.
     * Safe to call concurrently.
     * Safe to call with IORING_SETUP_SINGLE_ISSUER.
     * * @return true if the signal was sent, false on error (check errno)
     */
    bool Notify() const noexcept
    {
        if (wake_fd_ == -1)
        {
            return false;
        }
        constexpr uint64_t val = 1;
        // Direct write to eventfd is thread-safe and doesn't touch the ring
        return ::write(wake_fd_, &val, sizeof(val)) == sizeof(val);
    }

    // -------------------------------------------------------------------------
    // Operation Tracking
    // -------------------------------------------------------------------------

    void Track(OperationState* op)
    {
        AssertOwnerThread();

        op->tracked = true;
        op->next = pending_head_;
        op->prev = nullptr;
        if (pending_head_ != nullptr)
        {
            pending_head_->prev = op;
        }
        pending_head_ = op;
    }

    void Untrack(OperationState* op)
    {
        AssertOwnerThread();

        if (op->prev != nullptr)
        {
            op->prev->next = op->next;
        }
        else if (pending_head_ == op)
        {
            pending_head_ = op->next;
        }
        if (op->next)
        {
            op->next->prev = op->prev;
        }
        op->next = nullptr;
        op->prev = nullptr;
        op->tracked = false;
    }

    /**
     * Cancel all pending operations and drain completions.
     * Called automatically on destruction.
     *
     * IMPORTANT: This does NOT resume coroutines. Handles are dropped.
     */
    void CancelAllPending()
    {
        // Submit cancel requests for all tracked operations
        for (const auto* op = pending_head_; op; op = op->next)
        {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                io_uring_submit(&ring_);
                sqe = io_uring_get_sqe(&ring_);
                if (sqe == nullptr)
                {
                    break;
                }
            }
            io_uring_prep_cancel(sqe, op, 0);
            io_uring_sqe_set_data(sqe, nullptr);
        }
        io_uring_submit(&ring_);

        // Drain until all operations are untracked
        DrainWithoutResume();
    }

    // -------------------------------------------------------------------------
    // File Registration
    // -------------------------------------------------------------------------

    Result<> RegisterFiles(const std::span<const int> fds)
    {
        AssertOwnerThread();

        if (const int ret = io_uring_register_files(&ring_, fds.data(), fds.size()); ret < 0)
        {
            return ErrorFromErrno(-ret);
        }
        return {};
    }

    // -------------------------------------------------------------------------
    // Event Loop
    // -------------------------------------------------------------------------

    template <typename T>
    void RunUntilDone(Task<T>& t)
    {
        AssertOwnerThread();

        running_ = true;
        t.resume();
        while (running_ && !t.Done())
        {
            Step();
        }

        // signal stopped
        stopped_latch_.count_down();
    }

    void Run()
    {
        AssertOwnerThread();

        running_ = true;
        while (running_)
        {
            Step();
        }
    }

    template <typename Tick>
    void Run(Tick&& tick)
    {
        AssertOwnerThread();

        running_ = true;
        while (running_)
        {
            Step();
            tick();
        }

        stopped_latch_.count_down();
    }

    void Stop() { running_ = false; }

    // ---------------------------------------------------------------------
    // Cross-thread completion injection (for blocking pool offload, etc.)
    // ---------------------------------------------------------------------

    /**
     * Enqueue a completed operation that must be resumed on this io_context
     * thread. Returns true if the caller should send a signal to WakeFd()
     * to ensure the event loop wakes up.
     */
    bool EnqueueExternalDone(OperationState* op)
    {
        std::scoped_lock lk(ext_mtx_);
        ext_done_.push_back(op);
        ext_hint_.store(true, std::memory_order_relaxed);
        if (!ext_wake_pending_)
        {
            ext_wake_pending_ = true;
            return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // Low-level Access
    // -------------------------------------------------------------------------

    io_uring* Ring() { return &ring_; }
    int RingFd() const { return ring_.ring_fd; }

    // Returns the file descriptor used to wake this context from other threads.
    int WakeFd() const { return wake_fd_; }

    /**
     * Get the pipe pool for sendfile operations.
     * Created lazily on first access.
     */
    PipePool& GetPipePool()
    {
        std::call_once(pipe_pool_flag_, [this] { pipe_pool_.emplace(4); });
        return *pipe_pool_;
    }

    void EnsureSqes(const unsigned n)
    {
        AssertOwnerThread();

        if (io_uring_sq_space_left(&ring_) < n)
        {
            io_uring_submit(&ring_);

            if (io_uring_sq_space_left(&ring_) < n)
            {
                throw std::runtime_error("SQ full after submit");
            }
        }
    }

    io_uring_sqe* GetSqe()
    {
        AssertOwnerThread();

        return io_uring_get_sqe(&ring_);
    }

    void WaitReady() const { ready_latch_.wait(); }
    void WaitStop() const { stopped_latch_.wait(); }

private:
    void DrainExternal(std::vector<std::coroutine_handle<>>& out)
    {
        if (!ext_hint_.load(std::memory_order_relaxed))
        {
            return;
        }

        std::vector<OperationState*> local;
        {
            std::scoped_lock lk(ext_mtx_);
            if (ext_done_.empty())
            {
                ext_wake_pending_ = false;
                ext_hint_.store(false, std::memory_order_relaxed);
                return;
            }
            local.swap(ext_done_);
            ext_wake_pending_ = false;
            ext_hint_.store(false, std::memory_order_relaxed);
        }

        for (auto* op : local)
        {
            if (op == nullptr)
            {
                continue;
            }
            Untrack(op);
            out.push_back(op->handle);
        }
    }

    void DrainExternalWithoutResume()
    {
        if (!ext_hint_.load(std::memory_order_relaxed))
        {
            return;
        }

        std::vector<OperationState*> local;
        {
            std::scoped_lock lk(ext_mtx_);
            if (ext_done_.empty())
            {
                ext_wake_pending_ = false;
                ext_hint_.store(false, std::memory_order_relaxed);
                return;
            }
            local.swap(ext_done_);
            ext_wake_pending_ = false;
            ext_hint_.store(false, std::memory_order_relaxed);
        }

        for (auto* op : local)
        {
            if (op == nullptr)
            {
                continue;
            }
            Untrack(op);
            op->handle = {};
        }
    }

    /**
     * Drain completions without resuming coroutines.
     * Used during destruction to safely untrack all pending ops.
     */
    // TODO: return error
    void DrainWithoutResume()
    {
        while (pending_head_ != nullptr)
        {
            io_uring_cqe* cqe = nullptr;
            // Retry on EINTR
            int ret = 0;
            do
            {
                ret = io_uring_wait_cqe(&ring_, &cqe);
            } while (ret == -EINTR);

            if (ret < 0)
            {
                // Unrecoverable error in destruction path
                break;
            }

            const auto ud = io_uring_cqe_get_data64(cqe);

            if (ud == detail::WAKE_TAG)
            {
                // A cross-thread wake. Drain any externally completed ops.
                DrainExternalWithoutResume();
            }
            else if (ud)
            {
                auto* op = reinterpret_cast<OperationState*>(static_cast<uintptr_t>(ud));
                Untrack(op);
                // Do NOT resume op->handle — we're draining, not running
            }
            io_uring_cqe_seen(&ring_, cqe);
        }
    }

    void SubmitWakeRead()
    {
        // Must ensure we have space, though inside Step we typically do.
        // We use a simple read on the eventfd.
        auto* sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
        {
            // Force flush if full? This is rare in typical loop usage.
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe)
                return;  // Should fatal error really // TODO
        }

        io_uring_prep_read(sqe, wake_fd_, &wake_buffer_, sizeof(wake_buffer_), 0);
        io_uring_sqe_set_data64(sqe, detail::WAKE_TAG);
    }

    void Step()
    {
        // Retry on EINTR
        int ret = 0;
        do
        {
            ret = io_uring_submit_and_wait(&ring_, 1);
        } while (ret == -EINTR);

        if (ret < 0)
        {
            // If we failed to wait (and it wasn't EINTR), we can't really proceed.
            // Returning here might spin the loop if the error persists,            // but throwing from Step() is also
            // aggressive. For now, we assume transient errors or fatal ones we can't fix.
            return;
        }

        ready_.clear();

        auto [_, saw_wake] = ProcessReadyCompletions();

        // If a pool thread (or any other producer) completed work for this
        // context, it will have pushed ops into ext_done_ and signaled WakeFd.
        if (saw_wake || ext_hint_.load(std::memory_order_relaxed))
        {
            DrainExternal(ready_);
        }

        // Resume outside CQE iteration (flat, no stack growth)
        for (auto h : ready_)
        {
            if (h && !h.done())
            {
                h.resume();
            }
        }
    }

    // process ready completions, return the number of completions processed
    // and if wake signal was seen for the caller to decide what to do about it
    std::pair<unsigned, bool> ProcessReadyCompletions()
    {
        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned count = 0;
        bool saw_wake = false;

        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            const auto user_data = io_uring_cqe_get_data64(cqe);

            if (user_data == 0)
            {
                continue;
            }

            if (user_data == detail::WAKE_TAG)
            {
                saw_wake = true;
                // Re-arm the wake mechanism immediately for next wait
                SubmitWakeRead();
                continue;
            }

            auto* op = reinterpret_cast<OperationState*>(static_cast<uintptr_t>(user_data));
            Untrack(op);
            op->res = cqe->res;
            ready_.push_back(op->handle);
        }

        io_uring_cq_advance(&ring_, count);

        return {count, saw_wake};
    }

    io_uring ring_{};
    std::vector<std::coroutine_handle<>> ready_;
    OperationState* pending_head_ = nullptr;
    std::atomic<bool> running_ = false;
    std::once_flag pipe_pool_flag_;
    std::latch ready_latch_{1};
    std::latch stopped_latch_{1};

    // Internal Wake Mechanism (eventfd)
    int wake_fd_ = -1;
    uint64_t wake_buffer_ = 0;

    // External completions (from blocking pool, etc.).
    std::mutex ext_mtx_;
    std::vector<OperationState*> ext_done_;
    bool ext_wake_pending_ = false;       // protected by ext_mtx_
    std::atomic<bool> ext_hint_ = false;  // fast-path hint (may be stale)

    // Lazy pipe pool for sendfile operations
    std::optional<PipePool> pipe_pool_;
};

// -----------------------------------------------------------------------------
// Base for Operations (explicit object parameter)
// -----------------------------------------------------------------------------

struct UringOp : OperationState
{
protected:
    explicit UringOp(IoContext* c) { ctx = c; }

    UringOp(UringOp&&) = default;

public:
    bool await_ready() const noexcept { return false; }

    void await_suspend(this auto& self, std::coroutine_handle<> h)
    {
        self.handle = h;
        auto* op = static_cast<OperationState*>(&self);
        self.ctx->Track(op);
        self.ctx->EnsureSqes(1);
        auto* sqe = self.ctx->GetSqe();
        self.PrepareSqe(sqe);
        io_uring_sqe_set_data(sqe, op);
    }

    // Default: return size_t for read/write style ops
    Result<size_t> await_resume()
    {
        if (res < 0)
        {
            return std::unexpected(MakeErrorCode(res));
        }
        return static_cast<size_t>(res);
    }

    template <typename Rep, typename Period>
    auto WithTimeout(this auto&& self, std::chrono::duration<Rep, Period> dur)
        requires std::is_rvalue_reference_v<decltype(self)> &&
                 (!std::is_const_v<std::remove_reference_t<decltype(self)>>);
};

// -----------------------------------------------------------------------------
// Signal Handling
// -----------------------------------------------------------------------------

/**
 * RAII wrapper for signalfd.
 *
 * Creates a BLOCKING signalfd suitable for io_uring. Do not use SFD_NONBLOCK
 * with io_uring — it causes spurious EAGAIN completions when no signal is
 * pending. io_uring handles the blocking internally.
 */
class SignalSet
{
public:
    SignalSet(std::initializer_list<int> sigs)
    {
        sigset_t mask;
        sigemptyset(&mask);
        for (const int s : sigs)
        {
            sigaddset(&mask, s);
        }

        if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0)
        {
            throw std::system_error(errno, std::system_category(), "sigprocmask");
        }

        // SFD_CLOEXEC but NOT SFD_NONBLOCK — io_uring handles blocking
        fd_ = signalfd(-1, &mask, SFD_CLOEXEC);
        if (fd_ < 0)
        {
            throw std::system_error(errno, std::system_category(), "signalfd");
        }
    }

    ~SignalSet()
    {
        if (fd_ > 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    SignalSet(const SignalSet&) = delete;
    SignalSet& operator=(const SignalSet&) = delete;

    int fd() const { return fd_; }

private:
    int fd_;
};

struct WaitSignalOp : UringOp
{
    int fd;
    signalfd_siginfo info{};

    WaitSignalOp(IoContext& ctx, int signal_fd) : UringOp(&ctx), fd(signal_fd) {}

    void PrepareSqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, &info, sizeof(info), 0); }

    Result<int> await_resume()
    {
        if (res < 0)
        {
            return std::unexpected(MakeErrorCode(res));
        }
        return static_cast<int>(info.ssi_signo);
    }
};

inline WaitSignalOp AsyncWaitSignal(IoContext& ctx, int signal_fd)
{
    return WaitSignalOp(ctx, signal_fd);
}

// -----------------------------------------------------------------------------
// Timeout Wrapper
// -----------------------------------------------------------------------------

template <typename Op>
struct WithTimeoutOp
{
    Op op;
    __kernel_timespec ts{};

    template <typename Rep, typename Period>
    WithTimeoutOp(Op&& o, std::chrono::duration<Rep, Period> dur) : op(std::move(o))
    {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        ts.tv_sec = ns / 1'000'000'000;
        ts.tv_nsec = ns % 1'000'000'000;
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h)
    {
        op.handle = h;
        op.ctx->Track(&op);
        op.ctx->EnsureSqes(2);

        // Main operation with IOSQE_IO_LINK
        auto* sqe_op = op.ctx->GetSqe();
        op.PrepareSqe(sqe_op);
        sqe_op->flags |= IOSQE_IO_LINK;
        io_uring_sqe_set_data(sqe_op, &op);

        // Linked timeout (user_data = nullptr so we skip its CQE)
        auto* sqe_timer = op.ctx->GetSqe();
        io_uring_prep_link_timeout(sqe_timer, &ts, 0);
        io_uring_sqe_set_data(sqe_timer, nullptr);
    }

    auto await_resume()
    {
        auto r = op.await_resume();
        // Translate ECANCELED to timed_out for clarity
        if (!r && r.error().value() == ECANCELED)
        {
            return decltype(r)(std::unexpected(std::make_error_code(std::errc::timed_out)));
        }
        return r;
    }
};

// Builder method to apply a timeout to an io operation
template <typename Rep, typename Period>
auto UringOp::WithTimeout(this auto&& self, std::chrono::duration<Rep, Period> dur)
    requires std::is_rvalue_reference_v<decltype(self)> &&
             (!std::is_const_v<std::remove_reference_t<decltype(self)>>)
{
    using Op = std::remove_cvref_t<decltype(self)>;
    return WithTimeoutOp<Op>(std::forward<decltype(self)>(self), dur);
}

// Standalone helper
template <typename Op, typename Rep, typename Period>
auto Timeout(Op&& op, std::chrono::duration<Rep, Period> dur)
{
    return WithTimeoutOp<std::remove_reference_t<Op>>(std::forward<Op>(op), dur);
}

}  // namespace aio
