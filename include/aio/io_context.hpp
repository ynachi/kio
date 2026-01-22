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

#include "aio/operation_base.hpp"
#include "aio/result.hpp"
#include "aio/task.hpp"

namespace aio
{

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

/// Reserved user_data value for a cross-thread wake via MSG_RING
constexpr uint64_t WAKE_TAG = 1;

// -----------------------------------------------------------------------------
// io_context - The Event Loop
// -----------------------------------------------------------------------------

class io_context
{
public:
    explicit io_context(unsigned entries = 256)
    {
        io_uring_params params{};
        // Note: SINGLE_ISSUER omitted since we don't enforce thread affinity.
        // Add it back with thread-id checks if you want the optimization.
        // params.flags = IORING_SETUP_COOP_TASKRUN;

        int ret = io_uring_queue_init_params(entries, &ring_, &params);
        if (ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params");
        }

        // Reduce allocations in the hot path.
        ready_.reserve(entries);
        ext_done_.reserve(entries);
    }

    ~io_context()
    {
        cancel_all_pending();
        io_uring_queue_exit(&ring_);
    }

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    // -------------------------------------------------------------------------
    // Operation Tracking
    // -------------------------------------------------------------------------

    void track(operation_state* op)
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

    void untrack(operation_state* op)
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
    void cancel_all_pending()
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
        drain_without_resume();
    }

    // -------------------------------------------------------------------------
    // File Registration
    // -------------------------------------------------------------------------

    void register_files(std::span<const int> fds)
    {
        int ret = io_uring_register_files(&ring_, fds.data(), fds.size());
        if (ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_register_files");
        }
    }

    // -------------------------------------------------------------------------
    // Event Loop
    // -------------------------------------------------------------------------

    template <typename T>
    void run_until_done(task<T>& t)
    {
        running_ = true;
        t.resume();
        while (running_ && !t.done())
        {
            step();
        }
    }

    void run()
    {
        running_ = true;
        while (running_)
        {
            step();
        }
    }

    template <typename Tick>
    void run(Tick&& tick)
    {
        running_ = true;
        while (running_)
        {
            step();
            tick();
        }
    }

    void stop() { running_ = false; }

    // ---------------------------------------------------------------------
    // Cross-thread completion injection (for blocking pool offload, etc.)
    // ---------------------------------------------------------------------

    /**
     * Enqueue a completed operation that must be resumed on this io_context
     * thread. Returns true if the caller should send a WAKE_TAG (MSG_RING)
     * to ensure the event loop wakes up.
     */
    bool enqueue_external_done(operation_state* op)
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

    io_uring* ring() { return &ring_; }
    int ring_fd() const { return ring_.ring_fd; }

    void ensure_sqes(const unsigned n)
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

    io_uring_sqe* get_sqe() { return io_uring_get_sqe(&ring_); }

private:
    void drain_external(std::vector<std::coroutine_handle<>>& out)
    {
        if (!ext_hint_.load(std::memory_order_relaxed))
        {
            return;
        }

        std::vector<operation_state*> local;
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
            untrack(op);
            out.push_back(op->handle);
        }
    }

    void drain_external_without_resume()
    {
        if (!ext_hint_.load(std::memory_order_relaxed))
        {
            return;
        }

        std::vector<operation_state*> local;
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
            untrack(op);
            op->handle = {};
        }
    }

    /**
     * Drain completions without resuming coroutines.
     * Used during destruction to safely untrack all pending ops.
     */
    void drain_without_resume()
    {
        while (pending_head_)
        {
            io_uring_cqe* cqe;
            if (int ret = io_uring_wait_cqe(&ring_, &cqe); ret < 0)
            {
                // Error waiting — can't safely continue
                // In practice this shouldn't happen
                break;
            }

            auto ud = io_uring_cqe_get_data64(cqe);
            if (ud == WAKE_TAG)
            {
                // A cross-thread wake. Drain any externally completed ops.
                drain_external_without_resume();
            }
            else if (ud)
            {
                auto* op = reinterpret_cast<operation_state*>(static_cast<uintptr_t>(ud));
                untrack(op);
                // Do NOT resume op->handle — we're draining, not running
            }
            io_uring_cqe_seen(&ring_, cqe);
        }
    }

    void step()
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

            auto* op = reinterpret_cast<operation_state*>(static_cast<uintptr_t>(user_data));
            untrack(op);
            op->res = cqe->res;
            ready_.push_back(op->handle);
        }

        io_uring_cq_advance(&ring_, count);

        // If a pool thread (or any other producer) completed work for this
        // context, it will have pushed ops into ext_done_ and sent WAKE_TAG.
        // Drain them and resume their coroutines on this thread.
        if (saw_wake || ext_hint_.load(std::memory_order_relaxed))
        {
            drain_external(ready_);
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
    operation_state* pending_head_ = nullptr;
    std::atomic<bool> running_ = false;

    // External completions (from blocking pool, etc.).
    std::mutex ext_mtx_;
    std::vector<operation_state*> ext_done_;
    bool ext_wake_pending_ = false;       // protected by ext_mtx_
    std::atomic<bool> ext_hint_ = false;  // fast-path hint (may be stale)
};

// -----------------------------------------------------------------------------
// CRTP Base for Operations
// -----------------------------------------------------------------------------

template <typename Derived>
struct uring_op : operation_state
{
private:
    explicit uring_op(io_context* c) { ctx = c; }

    // Movable (delegates to operation_state move ctor)
    uring_op(uring_op&&) = default;

public:
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        ctx->track(this);
        ctx->ensure_sqes(1);
        auto* sqe = ctx->get_sqe();
        static_cast<Derived*>(this)->prepare_sqe(sqe);
        io_uring_sqe_set_data(sqe, this);
    }

    // Default: return size_t for read/write style ops
    Result<size_t> await_resume()
    {
        if (res < 0)
            return std::unexpected(make_error_code(res));
        return static_cast<size_t>(res);
    }

    template <typename Rep, typename Period>
    auto with_timeout(std::chrono::duration<Rep, Period> dur) &&;
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
struct async_msg_ring : uring_op<async_msg_ring>
{
    int target_fd;
    uint32_t msg_result;     // Target sees this as cqe->res
    uint64_t msg_user_data;  // Target sees this as cqe->user_data

    async_msg_ring(io_context* ctx, int target_ring_fd, uint32_t result, uint64_t user_data)
        : uring_op(ctx), target_fd(target_ring_fd), msg_result(result), msg_user_data(user_data)
    {
    }

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_msg_ring(sqe, target_fd, msg_result, msg_user_data, 0); }

    Result<void> await_resume()
    {
        if (res < 0)
            return std::unexpected(make_error_code(res));
        return {};
    }
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
class signal_set
{
public:
    signal_set(std::initializer_list<int> sigs)
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

    ~signal_set() { ::close(fd_); }

    signal_set(const signal_set&) = delete;
    signal_set& operator=(const signal_set&) = delete;

    int fd() const { return fd_; }

private:
    int fd_;
};

struct async_wait_signal : uring_op<async_wait_signal>
{
    int fd;
    signalfd_siginfo info{};

    async_wait_signal(io_context* ctx, int signal_fd) : uring_op(ctx), fd(signal_fd) {}

    void prepare_sqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, &info, sizeof(info), 0); }

    Result<int> await_resume()
    {
        if (res < 0)
            return std::unexpected(make_error_code(res));
        return static_cast<int>(info.ssi_signo);
    }
};

// -----------------------------------------------------------------------------
// Timeout Wrapper
// -----------------------------------------------------------------------------

template <typename Op>
struct with_timeout_op
{
    Op op;
    __kernel_timespec ts{};

    template <typename Rep, typename Period>
    with_timeout_op(Op&& o, std::chrono::duration<Rep, Period> dur) : op(std::move(o))
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

// Define the builder method now that with_timeout_op is complete
template <typename Derived>
template <typename Rep, typename Period>
auto uring_op<Derived>::with_timeout(std::chrono::duration<Rep, Period> dur) &&
{
    return with_timeout_op<Derived>(std::move(*static_cast<Derived*>(this)), dur);
}

// Standalone helper
template <typename Op, typename Rep, typename Period>
auto timeout(Op&& op, std::chrono::duration<Rep, Period> dur)
{
    return with_timeout_op<std::remove_reference_t<Op>>(std::forward<Op>(op), dur);
}

}  // namespace aio