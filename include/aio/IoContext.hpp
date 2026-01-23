#pragma once

/**
 * aio.hpp - Minimal async I/O library built on io_uring and C++23 coroutines
 *
 * Design:
 *   - Thread-per-core model: one io_context per thread, no cross-thread submission
 *   - Coroutine handle stored in user_data for direct resume
 *   - Intrusive tracking of pending operations for safe cancellation/shutdown
 *   - Caller owns buffers; library does not allocate
 *
 * Safety:
 *   Operation lifetimes are tracked via an intrusive linked list. If a coroutine
 *   frame is destroyed while an I/O operation is still pending (kernel hasn't
 *   completed it), the program will terminate rather than risk silent memory
 *   corruption. This is detection, not prevention.
 *
 *   !! IMPORTANT !!
 *   Operations are embedded in coroutine frames. If you destroy a task while its
 *   coroutine is suspended on I/O, you WILL hit std::terminate(). This is
 *   intentional — the alternative is silent memory corruption when the kernel
 *   writes to freed memory.
 *
 *   To safely cancel work:
 *   1. Use timeouts (.with_timeout()) so operations eventually complete
 *   2. Let io_context::cancel_all_pending() drain on destruction
 *   3. Don't destroy tasks with pending I/O — wait for them to complete
 *
 *   A future version may use a slab allocator to fully prevent UAF, but that
 *   adds complexity inappropriate for this minimal implementation.
 *
 * Requirements:
 *   - Linux kernel >= 6.0
 *   - liburing
 *   - C++23
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <csignal>
#include <cstring>
#include <exception>
#include <expected>
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

#include "aio/Task.hpp"
#include "aio/operation_base.hpp"
#include "aio/result.hpp"

namespace aio
{

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

/// Reserved user_data value for a cross-thread wake via MSG_RING
constexpr uint64_t WAKE_TAG = 1;

// -----------------------------------------------------------------------------
// IoContext - The Event Loop
// -----------------------------------------------------------------------------

class IoContext
{
public:
    explicit IoContext(unsigned entries = 256)
    {
        io_uring_params params{};
        // Note: SINGLE_ISSUER omitted since we don't enforce thread affinity.
        // Add it back with thread-id checks if you want the optimization.
        // params.flags = IORING_SETUP_COOP_TASKRUN;

        if (int ret = io_uring_queue_init_params(entries, &ring_, &params); ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params");
        }

        // Reduce allocations in the hot path.
        ready_.reserve(entries);
        ext_done_.reserve(entries);
    }

    ~IoContext()
    {
        CancelAllPending();
        io_uring_queue_exit(&ring_);
    }

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;

    // -------------------------------------------------------------------------
    // Operation Tracking
    // -------------------------------------------------------------------------

    void Track(OperationState* op)
    {
        op->tracked = true;
        op->next = pending_head_;
        op->prev = nullptr;
        if (pending_head_)
        {
            pending_head_->prev = op;
        }
        pending_head_ = op;
    }

    void Untrack(OperationState* op)
    {
        if (op->prev)
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
     * This is safe during destruction but means coroutines won't see
     * their final results. Use stop() + run() drain for graceful shutdown.
     *
     * Note: IORING_OP_ASYNC_CANCEL is itself async and may fail (e.g., if
     * the operation already completed). We handle this by draining until
     * pending_head_ is empty, regardless of cancel success/failure.
     */
    void CancelAllPending()
    {
        // Submit cancel requests for all tracked operations
        for (auto* op = pending_head_; op; op = op->next)
        {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe)
            {
                io_uring_submit(&ring_);
                sqe = io_uring_get_sqe(&ring_);
                if (!sqe)
                    break;
            }
            io_uring_prep_cancel(sqe, op, 0);
            io_uring_sqe_set_data(sqe, nullptr);
        }
        io_uring_submit(&ring_);

        // Drain until all operations are untracked
        // Do NOT resume coroutines — we're in destruction
        DrainWithoutResume();
    }

    // -------------------------------------------------------------------------
    // File Registration
    // -------------------------------------------------------------------------

    void RegisterFiles(std::span<const int> fds)
    {
        int ret = io_uring_register_files(&ring_, fds.data(), fds.size());
        if (ret < 0)
        {
            // TODO: not fatal, return result instead
            throw std::system_error(-ret, std::system_category(), "io_uring_register_files");
        }
    }

    // -------------------------------------------------------------------------
    // Event Loop
    // -------------------------------------------------------------------------

    template <typename T>
    void RunUntilDone(Task<T>& t)
    {
        running_ = true;
        t.resume();
        while (running_ && !t.done())
        {
            Step();
        }
    }

    void Run()
    {
        running_ = true;
        while (running_)
        {
            Step();
        }
    }

    template <typename Tick>
    void Run(Tick&& tick)
    {
        running_ = true;
        while (running_)
        {
            Step();
            tick();
        }
    }

    void Stop() { running_ = false; }

    // ---------------------------------------------------------------------
    // Cross-thread completion injection (for blocking pool offload, etc.)
    // ---------------------------------------------------------------------

    /**
     * Enqueue a completed operation that must be resumed on this io_context
     * thread. Returns true if the caller should send a WAKE_TAG (MSG_RING)
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

    // TODO: maybe private ?
    io_uring* Ring() { return &ring_; }
    int RingFd() const { return ring_.ring_fd; }

    void EnsureSqes(const unsigned n)
    {
        if (io_uring_sq_space_left(&ring_) < n)
        {
            io_uring_submit(&ring_);
            if (io_uring_sq_space_left(&ring_) < n)
            {
                throw std::runtime_error("SQ full after submit");
            }
        }
    }

    io_uring_sqe* GetSqe() { return io_uring_get_sqe(&ring_); }

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
            if (!op)
                continue;
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
            if (!op)
                continue;
            Untrack(op);
            op->handle = {};
        }
    }

    /**
     * Drain completions without resuming coroutines.
     * Used during destruction to safely untrack all pending ops.
     */
    void DrainWithoutResume()
    {
        while (pending_head_ != nullptr)
        {
            io_uring_cqe* cqe = nullptr;
            if (int ret = io_uring_wait_cqe(&ring_, &cqe); ret < 0)
            {
                // Error waiting — can't safely continue
                // In practice this shouldn't happen
                break;
            }

            if (const auto ud = io_uring_cqe_get_data64(cqe); ud == WAKE_TAG)
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

    void Step()
    {
        io_uring_submit_and_wait(&ring_, 1);

        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned count = 0;

        ready_.clear();

        bool saw_wake = false;

        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            auto user_data = io_uring_cqe_get_data64(cqe);

            if (user_data == 0)
            {
                continue;
            }

            if (user_data == WAKE_TAG)
            {
                saw_wake = true;
                continue;
            }

            auto* op = reinterpret_cast<OperationState*>(static_cast<uintptr_t>(user_data));
            Untrack(op);
            op->res = cqe->res;
            ready_.push_back(op->handle);
        }

        io_uring_cq_advance(&ring_, count);

        // If a pool thread (or any other producer) completed work for this
        // context, it will have pushed ops into ext_done_ and sent WAKE_TAG.
        // Drain them and resume their coroutines on this thread.
        if (saw_wake || ext_hint_.load(std::memory_order_relaxed))
        {
            DrainExternal(ready_);
        }

        // Resume outside CQE iteration (flat, no stack growth)
        for (auto h : ready_)
        {
            if (h && !h.done())
                h.resume();
        }
    }

    io_uring ring_{};
    std::vector<std::coroutine_handle<>> ready_;
    OperationState* pending_head_ = nullptr;
    // TODO: use std::stop_token
    std::atomic<bool> running_ = false;

    // External completions (from blocking pool, etc.).
    std::mutex ext_mtx_;
    std::vector<OperationState*> ext_done_;
    bool ext_wake_pending_ = false;       // protected by ext_mtx_
    std::atomic<bool> ext_hint_ = false;  // fast-path hint (may be stale)
};

// -----------------------------------------------------------------------------
// CRTP Base for Operations
// -----------------------------------------------------------------------------

template <typename Derived>
struct UringOp : OperationState
{
private:
    explicit UringOp(IoContext* c) { ctx = c; }

    // Movable (delegates to operation_state move ctor)
    UringOp(UringOp&&) = default;

public:
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        ctx->Track(this);
        ctx->EnsureSqes(1);
        auto* sqe = ctx->GetSqe();
        static_cast<Derived*>(this)->prepare_sqe(sqe);
        io_uring_sqe_set_data(sqe, this);
    }

    // Default: return size_t for read/write style ops
    Result<size_t> await_resume()
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return static_cast<size_t>(res);
    }

    template <typename Rep, typename Period>
    auto WithTimeout(std::chrono::duration<Rep, Period> dur) &&;
    friend Derived;
};

// -----------------------------------------------------------------------------
// Concrete Operations: Cross-thread (MSG_RING)
// -----------------------------------------------------------------------------

/**
 * Send a message to another io_uring ring via MSG_RING.
 *
 * The target ring will receive a CQE with:
 *   - cqe->res = msg_result (32-bit)
 *   - cqe->user_data = msg_user_data (64-bit)
 *
 * Use WAKE_TAG as msg_user_data if you just want to wake the target
 * without triggering any operation completion logic.
 */
struct MsgRingOp : UringOp<MsgRingOp>
{
    int target_fd;
    uint32_t msg_result;     // Target sees this as cqe->res
    uint64_t msg_user_data;  // Target sees this as cqe->user_data

    MsgRingOp(IoContext& ctx, int target_ring_fd, uint32_t result, uint64_t user_data)
        : UringOp(&ctx), target_fd(target_ring_fd), msg_result(result), msg_user_data(user_data)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_msg_ring(sqe, target_fd, msg_result, msg_user_data, 0); }

    Result<void> await_resume()
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
        return {};
    }
};

inline MsgRingOp async_msg_ring(IoContext& ctx, int target_ring_fd, uint32_t result, uint64_t user_data)
{
    return MsgRingOp(ctx, target_ring_fd, result, user_data);
}

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
        for (int s : sigs)
            sigaddset(&mask, s);

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

    ~SignalSet() { ::close(fd_); }

    SignalSet(const SignalSet&) = delete;
    SignalSet& operator=(const SignalSet&) = delete;

    int fd() const { return fd_; }

private:
    int fd_;
};

struct WaitSignalOp : UringOp<WaitSignalOp>
{
    int fd;
    signalfd_siginfo info{};

    WaitSignalOp(IoContext& ctx, int signal_fd) : UringOp(&ctx), fd(signal_fd) {}

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, &info, sizeof(info), 0); }

    Result<int> await_resume()
    {
        if (res < 0)
            return std::unexpected(MakeErrorCode(res));
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
        op.ctx->track(&op);
        op.ctx->ensure_sqes(2);

        // Main operation with IOSQE_IO_LINK
        auto* sqe_op = op.ctx->get_sqe();
        op.prepare_sqe(sqe_op);
        sqe_op->flags |= IOSQE_IO_LINK;
        io_uring_sqe_set_data(sqe_op, &op);

        // Linked timeout (user_data = nullptr so we skip its CQE)
        auto* sqe_timer = op.ctx->get_sqe();
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
template <typename Derived>
template <typename Rep, typename Period>
auto UringOp<Derived>::WithTimeout(std::chrono::duration<Rep, Period> dur) &&
{
    return WithTimeoutOp<Derived>(std::move(*static_cast<Derived*>(this)), dur);
}

// Standalone helper
template <typename Op, typename Rep, typename Period>
auto Timeout(Op&& op, std::chrono::duration<Rep, Period> dur)
{
    return WithTimeoutOp<std::remove_reference_t<Op>>(std::forward<Op>(op), dur);
}

}  // namespace aio