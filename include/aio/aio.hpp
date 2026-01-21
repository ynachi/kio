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

#include <liburing.h>
#include <coroutine>
#include <expected>
#include <chrono>
#include <vector>
#include <span>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <optional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <exception>
#include <sys/signalfd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

#include "aio/result.hpp"

namespace aio {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

/// Reserved user_data value for cross-thread wake via MSG_RING
constexpr uint64_t WAKE_TAG = 1;

// -----------------------------------------------------------------------------
// Forward Declarations
// -----------------------------------------------------------------------------

class io_context;
template<typename Op> struct with_timeout_op;

// -----------------------------------------------------------------------------
// Operation State (Intrusive Tracking)
// -----------------------------------------------------------------------------

/**
 * Base state for all pending I/O operations.
 *
 * Linked into io_context's pending list on submission, unlinked on completion.
 * If destroyed while still linked, the program terminates — this catches bugs
 * where a coroutine frame is destroyed while its I/O is still in flight.
 *
 * WARNING: This is detection, not prevention. The operation is embedded in the
 * coroutine frame, so destroying the task destroys the operation. The terminate()
 * is a fail-fast to avoid silent memory corruption.
 *
 * Movable only when not tracked (before await_suspend).
 */
struct operation_state {
    io_context* ctx = nullptr;
    int32_t res = 0;
    std::coroutine_handle<> handle;

    // Intrusive doubly-linked list pointers
    operation_state* next = nullptr;
    operation_state* prev = nullptr;
    bool tracked = false;

    operation_state() = default;

    // Move allowed only when not tracked
    operation_state(operation_state&& other) noexcept
        : ctx(other.ctx), res(other.res), handle(other.handle),
          next(nullptr), prev(nullptr), tracked(false) {
        // Source must not be tracked
        if (other.tracked) {
            std::terminate();
        }
        other.ctx = nullptr;
        other.handle = nullptr;
    }

    operation_state& operator=(operation_state&&) = delete;
    operation_state(const operation_state&) = delete;
    operation_state& operator=(const operation_state&) = delete;

    ~operation_state() {
        if (tracked) {
            // Coroutine destroyed while I/O pending → memory corruption risk
            // Terminate loudly rather than corrupt silently
            std::terminate();
        }
    }
};

// -----------------------------------------------------------------------------
// task<T> - Minimal Coroutine Return Type
// -----------------------------------------------------------------------------

template<typename T = void>
class task {
public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        task get_return_object() {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation)
                    return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type h) : handle_(h) {}
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~task() noexcept { if (handle_) handle_.destroy(); }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    bool done() const { return handle_ && handle_.done(); }

    T result() {
        if (handle_.promise().exception)
            std::rethrow_exception(handle_.promise().exception);
        return std::move(*handle_.promise().value);
    }

    void resume() { if (handle_ && !handle_.done()) handle_.resume(); }

    // Alias resume for clarity: Starts the task concurrently.
    // WARNING: You must still keep the 'task' object alive!
    void start() { resume(); }

    // Awaitable interface
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) noexcept {
        handle_.promise().continuation = cont;
        return handle_;
    }

    T await_resume() {
        if (handle_.promise().exception)
            std::rethrow_exception(handle_.promise().exception);
        return std::move(*handle_.promise().value);
    }

private:
    handle_type handle_;
};

template<>
class task<void> {
public:
    struct promise_type {
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        task get_return_object() {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation)
                    return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type h) : handle_(h) {}
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~task() { if (handle_) handle_.destroy(); }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    bool done() const { return handle_ && handle_.done(); }

    void result() {
        if (handle_.promise().exception)
            std::rethrow_exception(handle_.promise().exception);
    }

    void resume() { if (handle_ && !handle_.done()) handle_.resume(); }

    // Alias resume for clarity: Starts the task concurrently.
    // WARNING: You must still keep the 'task' object alive!
    void start() { resume(); }

    // Awaitable interface
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) noexcept {
        handle_.promise().continuation = cont;
        return handle_;
    }

    void await_resume() {
        if (handle_.promise().exception)
            std::rethrow_exception(handle_.promise().exception);
    }

private:
    handle_type handle_;
};

// -----------------------------------------------------------------------------
// io_context - The Event Loop
// -----------------------------------------------------------------------------

class io_context {
public:
    explicit io_context(unsigned entries = 256) {
        io_uring_params params{};
        // Note: SINGLE_ISSUER omitted since we don't enforce thread affinity.
        // Add it back with thread-id checks if you want the optimization.
        //params.flags = IORING_SETUP_COOP_TASKRUN;

        int ret = io_uring_queue_init_params(entries, &ring_, &params);
        if (ret < 0) {
            throw std::system_error(-ret, std::system_category(),
                                    "io_uring_queue_init_params");
        }

        // Reduce allocations in the hot path.
        ready_.reserve(entries);
        ext_done_.reserve(entries);
    }

    ~io_context() {
        cancel_all_pending();
        io_uring_queue_exit(&ring_);
    }

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    // -------------------------------------------------------------------------
    // Operation Tracking
    // -------------------------------------------------------------------------

    void track(operation_state* op) {
        op->tracked = true;
        op->next = pending_head_;
        op->prev = nullptr;
        if (pending_head_) {
            pending_head_->prev = op;
        }
        pending_head_ = op;
    }

    void untrack(operation_state* op) {
        if (op->prev) {
            op->prev->next = op->next;
        } else if (pending_head_ == op) {
            pending_head_ = op->next;
        }
        if (op->next) {
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
    void cancel_all_pending() {
        // Submit cancel requests for all tracked operations
        for (auto* op = pending_head_; op; op = op->next) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) {
                io_uring_submit(&ring_);
                sqe = io_uring_get_sqe(&ring_);
                if (!sqe) break;
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

    void register_files(std::span<const int> fds) {
        int ret = io_uring_register_files(&ring_, fds.data(), fds.size());
        if (ret < 0) {
            throw std::system_error(-ret, std::system_category(),
                                    "io_uring_register_files");
        }
    }

    // -------------------------------------------------------------------------
    // Event Loop
    // -------------------------------------------------------------------------

    template<typename T>
    void run_until_done(task<T>& t) {
        running_ = true;
        t.resume();
        while (running_ && !t.done()) {
            step();
        }
    }

    void run() {
        running_ = true;
        while (running_) {
            step();
        }
    }

    template<typename Tick>
    void run(Tick&& tick) {
        running_ = true;
        while (running_) {
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
    bool enqueue_external_done(operation_state* op) {
        std::lock_guard<std::mutex> lk(ext_mtx_);
        ext_done_.push_back(op);
        ext_hint_.store(true, std::memory_order_relaxed);
        if (!ext_wake_pending_) {
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

    void ensure_sqes(unsigned n) {
        if (io_uring_sq_space_left(&ring_) < n) {
            io_uring_submit(&ring_);
            if (io_uring_sq_space_left(&ring_) < n) {
                throw std::runtime_error("SQ full after submit");
            }
        }
    }

    io_uring_sqe* get_sqe() {
        return io_uring_get_sqe(&ring_);
    }

private:
    void drain_external(std::vector<std::coroutine_handle<>>& out) {
        if (!ext_hint_.load(std::memory_order_relaxed)) {
            return;
        }

        std::vector<operation_state*> local;
        {
            std::lock_guard<std::mutex> lk(ext_mtx_);
            if (ext_done_.empty()) {
                ext_wake_pending_ = false;
                ext_hint_.store(false, std::memory_order_relaxed);
                return;
            }
            local.swap(ext_done_);
            ext_wake_pending_ = false;
            ext_hint_.store(false, std::memory_order_relaxed);
        }

        for (auto* op : local) {
            if (!op) continue;
            untrack(op);
            out.push_back(op->handle);
        }
    }

    void drain_external_without_resume() {
        if (!ext_hint_.load(std::memory_order_relaxed)) {
            return;
        }

        std::vector<operation_state*> local;
        {
            std::lock_guard<std::mutex> lk(ext_mtx_);
            if (ext_done_.empty()) {
                ext_wake_pending_ = false;
                ext_hint_.store(false, std::memory_order_relaxed);
                return;
            }
            local.swap(ext_done_);
            ext_wake_pending_ = false;
            ext_hint_.store(false, std::memory_order_relaxed);
        }

        for (auto* op : local) {
            if (!op) continue;
            untrack(op);
            op->handle = {};
        }
    }

    /**
     * Drain completions without resuming coroutines.
     * Used during destruction to safely untrack all pending ops.
     */
    void drain_without_resume() {
        while (pending_head_) {
            io_uring_cqe* cqe;
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret < 0) {
                // Error waiting — can't safely continue
                // In practice this shouldn't happen
                break;
            }

            auto ud = io_uring_cqe_get_data64(cqe);
            if (ud == WAKE_TAG) {
                // A cross-thread wake. Drain any externally completed ops.
                drain_external_without_resume();
            } else if (ud) {
                auto* op = reinterpret_cast<operation_state*>(static_cast<uintptr_t>(ud));
                untrack(op);
                // Do NOT resume op->handle — we're draining, not running
            }
            io_uring_cqe_seen(&ring_, cqe);
        }
    }

    void step() {
        io_uring_submit_and_wait(&ring_, 1);

        io_uring_cqe* cqe;
        unsigned head;
        unsigned count = 0;

        ready_.clear();

        bool saw_wake = false;

        io_uring_for_each_cqe(&ring_, head, cqe) {
            count++;
            auto user_data = io_uring_cqe_get_data64(cqe);

            if (user_data == 0) {
                continue;
            }

            if (user_data == WAKE_TAG) {
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
        if (saw_wake || ext_hint_.load(std::memory_order_relaxed)) {
            drain_external(ready_);
        }

        // Resume outside CQE iteration (flat, no stack growth)
        for (auto h : ready_) {
            if (h && !h.done()) h.resume();
        }
    }

    io_uring ring_;
    std::vector<std::coroutine_handle<>> ready_;
    operation_state* pending_head_ = nullptr;
    std::atomic<bool> running_ = false;

    // External completions (from blocking pool, etc.).
    std::mutex ext_mtx_;
    std::vector<operation_state*> ext_done_;
    bool ext_wake_pending_ = false; // protected by ext_mtx_
    std::atomic<bool> ext_hint_ = false; // fast-path hint (may be stale)
};

// -----------------------------------------------------------------------------
// CRTP Base for Operations
// -----------------------------------------------------------------------------

template<typename Derived>
struct uring_op : public operation_state {

    explicit uring_op(io_context* c) { ctx = c; }

    // Movable (delegates to operation_state move ctor)
    uring_op(uring_op&&) = default;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        ctx->track(this);
        ctx->ensure_sqes(1);
        auto* sqe = ctx->get_sqe();
        static_cast<Derived*>(this)->prepare_sqe(sqe);
        io_uring_sqe_set_data(sqe, this);
    }

    // Default: return size_t for read/write style ops
    Result<size_t> await_resume() {
        if (res < 0) return std::unexpected(make_error_code(res));
        return static_cast<size_t>(res);
    }

    template<typename Rep, typename Period>
    auto with_timeout(std::chrono::duration<Rep, Period> dur) &&;
};

// -----------------------------------------------------------------------------
// Concrete Operations: File I/O
// -----------------------------------------------------------------------------

struct async_read : uring_op<async_read> {
    int fd;
    void* buffer;
    size_t len;
    off_t offset;

    async_read(io_context* ctx, int fd, std::span<std::byte> buf, off_t off)
        : uring_op(ctx), fd(fd), buffer(buf.data()), len(buf.size()), offset(off) {}

    // Convenience: accept void* + size
    async_read(io_context* ctx, int fd, void* buf, size_t len, off_t off)
        : uring_op(ctx), fd(fd), buffer(buf), len(len), offset(off) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_read(sqe, fd, buffer, len, offset);
    }
};

struct async_read_fixed : uring_op<async_read_fixed> {
    int file_index;
    void* buffer;
    size_t len;
    off_t offset;

    async_read_fixed(io_context* ctx, int idx, std::span<std::byte> buf, off_t off)
        : uring_op(ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_read(sqe, file_index, buffer, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    }
};

struct async_write : uring_op<async_write> {
    int fd;
    const void* buffer;
    size_t len;
    off_t offset;

    async_write(io_context* ctx, int fd, std::span<const std::byte> buf, off_t off)
        : uring_op(ctx), fd(fd), buffer(buf.data()), len(buf.size()), offset(off) {}

    async_write(io_context* ctx, int fd, const void* buf, size_t len, off_t off)
        : uring_op(ctx), fd(fd), buffer(buf), len(len), offset(off) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_write(sqe, fd, buffer, len, offset);
    }
};

struct async_write_fixed : uring_op<async_write_fixed> {
    int file_index;
    const void* buffer;
    size_t len;
    off_t offset;

    async_write_fixed(io_context* ctx, int idx, std::span<const std::byte> buf, off_t off)
        : uring_op(ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_write(sqe, file_index, buffer, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    }
};

// -----------------------------------------------------------------------------
// Concrete Operations: Networking
// -----------------------------------------------------------------------------

struct async_accept : uring_op<async_accept> {
    int fd;
    sockaddr_storage addr{};
    socklen_t addrlen = sizeof(addr);

    async_accept(io_context* ctx, int fd) : uring_op(ctx), fd(fd) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_accept(sqe, fd, reinterpret_cast<sockaddr*>(&addr), &addrlen, 0);
    }

    Result<int> await_resume() {
        if (res < 0) return std::unexpected(make_error_code(res));
        return res;
    }
};

struct async_connect : uring_op<async_connect> {
    int fd;
    sockaddr_storage addr_store{};
    socklen_t addrlen;

    async_connect(io_context* ctx, int fd, const sockaddr* addr, socklen_t len)
        : uring_op(ctx), fd(fd), addrlen(len) {
        std::memcpy(&addr_store, addr, len);
    }

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_connect(sqe, fd, reinterpret_cast<sockaddr*>(&addr_store), addrlen);
    }

    Result<void> await_resume() {
        if (res < 0) return std::unexpected(make_error_code(res));
        return {};
    }
};

struct async_recv : uring_op<async_recv> {
    int fd;
    void* buffer;
    size_t len;
    int flags;

    async_recv(io_context* ctx, int fd, void* buf, size_t len, int flags = 0)
        : uring_op(ctx), fd(fd), buffer(buf), len(len), flags(flags) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_recv(sqe, fd, buffer, len, flags);
    }
};

struct async_send : uring_op<async_send> {
    int fd;
    const void* buffer;
    size_t len;
    int flags;

    async_send(io_context* ctx, int fd, const void* buf, size_t len, int flags = 0)
        : uring_op(ctx), fd(fd), buffer(buf), len(len), flags(flags) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_send(sqe, fd, buffer, len, flags);
    }
};

struct async_close : uring_op<async_close> {
    int fd;

    async_close(io_context* ctx, int fd) : uring_op(ctx), fd(fd) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_close(sqe, fd);
    }

    Result<void> await_resume() {
        if (res < 0) return std::unexpected(make_error_code(res));
        return {};
    }
};

// -----------------------------------------------------------------------------
// Concrete Operations: Timing
// -----------------------------------------------------------------------------

struct async_sleep : uring_op<async_sleep> {
    __kernel_timespec ts;

    template<typename Rep, typename Period>
    async_sleep(io_context* ctx, std::chrono::duration<Rep, Period> dur)
        : uring_op(ctx) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        ts.tv_sec = ns / 1'000'000'000;
        ts.tv_nsec = ns % 1'000'000'000;
    }

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_timeout(sqe, &ts, 0, 0);
    }

    Result<void> await_resume() {
        // -ETIME is expected for timeout completion
        if (res == -ETIME) return {};
        if (res < 0) return std::unexpected(make_error_code(res));
        return {};
    }
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
struct async_msg_ring : uring_op<async_msg_ring> {
    int target_fd;
    uint32_t msg_result;       // Target sees this as cqe->res
    uint64_t msg_user_data;    // Target sees this as cqe->user_data

    async_msg_ring(io_context* ctx, int target_ring_fd,
                   uint32_t result, uint64_t user_data)
        : uring_op(ctx), target_fd(target_ring_fd),
          msg_result(result), msg_user_data(user_data) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_msg_ring(sqe, target_fd, msg_result, msg_user_data, 0);
    }

    Result<void> await_resume() {
        if (res < 0) return std::unexpected(make_error_code(res));
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
class signal_set {
public:
    signal_set(std::initializer_list<int> sigs) {
        sigset_t mask;
        sigemptyset(&mask);
        for (int s : sigs) sigaddset(&mask, s);

        if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
            throw std::system_error(errno, std::system_category(), "sigprocmask");
        }

        // SFD_CLOEXEC but NOT SFD_NONBLOCK — io_uring handles blocking
        fd_ = signalfd(-1, &mask, SFD_CLOEXEC);
        if (fd_ < 0) {
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

struct async_wait_signal : uring_op<async_wait_signal> {
    int fd;
    signalfd_siginfo info{};

    async_wait_signal(io_context* ctx, int signal_fd)
        : uring_op(ctx), fd(signal_fd) {}

    void prepare_sqe(io_uring_sqe* sqe) {
        io_uring_prep_read(sqe, fd, &info, sizeof(info), 0);
    }

    Result<int> await_resume() {
        if (res < 0) return std::unexpected(make_error_code(res));
        return static_cast<int>(info.ssi_signo);
    }
};

// -----------------------------------------------------------------------------
// Timeout Wrapper
// -----------------------------------------------------------------------------

template<typename Op>
struct with_timeout_op {
    Op op;
    __kernel_timespec ts;

    template<typename Rep, typename Period>
    with_timeout_op(Op&& o, std::chrono::duration<Rep, Period> dur)
        : op(std::move(o)) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        ts.tv_sec = ns / 1'000'000'000;
        ts.tv_nsec = ns % 1'000'000'000;
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
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

    auto await_resume() {
        auto r = op.await_resume();
        // Translate ECANCELED to timed_out for clarity
        if (!r && r.error().value() == ECANCELED) {
            return decltype(r)(std::unexpected(
                std::make_error_code(std::errc::timed_out)));
        }
        return r;
    }
};

// Define the builder method now that with_timeout_op is complete
template<typename Derived>
template<typename Rep, typename Period>
auto uring_op<Derived>::with_timeout(std::chrono::duration<Rep, Period> dur) && {
    return with_timeout_op<Derived>(std::move(*static_cast<Derived*>(this)), dur);
}

// Standalone helper
template<typename Op, typename Rep, typename Period>
auto timeout(Op&& op, std::chrono::duration<Rep, Period> dur) {
    return with_timeout_op<std::remove_reference_t<Op>>(std::forward<Op>(op), dur);
}

// -----------------------------------------------------------------------------
// Blocking Pool + Offload (for DNS, disk metadata, etc.)
// -----------------------------------------------------------------------------

namespace detail {

// Pool threads need a way to wake an io_context thread that might be sleeping
// in io_uring_submit_and_wait(). We do that by sending a MSG_RING with
// user_data = WAKE_TAG. To avoid cross-thread submissions into the worker's
// ring, each pool thread owns a tiny io_uring used only for MSG_RING.
class msg_ring_waker {
public:
    msg_ring_waker() {
        int ret = io_uring_queue_init(32, &ring_, 0);
        if (ret < 0) {
            throw std::system_error(-ret, std::system_category(), "msg_ring_waker: io_uring_queue_init");
        }
    }

    ~msg_ring_waker() {
        io_uring_queue_exit(&ring_);
    }

    msg_ring_waker(const msg_ring_waker&) = delete;
    msg_ring_waker& operator=(const msg_ring_waker&) = delete;

    void wake(int target_ring_fd) noexcept {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            (void)io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) return; // best-effort
        }

        // Target will receive a CQE with user_data=WAKE_TAG.
        io_uring_prep_msg_ring(sqe, target_ring_fd, /*res*/ 0u, /*user_data*/ WAKE_TAG, 0);
        io_uring_sqe_set_data(sqe, nullptr);
        (void)io_uring_submit(&ring_);

        // Reap CQEs so this tiny ring doesn't fill up.
        io_uring_cqe* cqes[32];
        unsigned n = io_uring_peek_batch_cqe(&ring_, cqes, 32);
        if (n) io_uring_cq_advance(&ring_, n);
    }

private:
    io_uring ring_{};
};

inline thread_local msg_ring_waker tls_waker;

} // namespace detail

class blocking_pool {
public:
    struct job {
        void (*fn)(void*) noexcept;
        void* arg;
    };

    explicit blocking_pool(std::size_t threads,
                           std::size_t capacity = 4096)
        : cap_(capacity), q_(capacity) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~blocking_pool() {
        stop();
    }

    blocking_pool(const blocking_pool&) = delete;
    blocking_pool& operator=(const blocking_pool&) = delete;

    void stop() {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            cv_.notify_all();
        }
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    // Non-blocking submission (safe for io_uring worker threads).
    // Returns false if the queue is full.
    bool try_submit(job j) {
        std::lock_guard<std::mutex> lk(m_);
        if (count_ == cap_) return false;
        q_[tail_] = j;
        tail_ = (tail_ + 1) % cap_;
        ++count_;
        cv_.notify_one();
        return true;
    }

private:
    void worker_loop() {
        while (true) {
            job j{};
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] {
                    return stopping_.load(std::memory_order_relaxed) || count_ > 0;
                });
                if (stopping_.load(std::memory_order_relaxed) && count_ == 0) {
                    return;
                }
                j = q_[head_];
                head_ = (head_ + 1) % cap_;
                --count_;
            }
            j.fn(j.arg);
        }
    }

    std::atomic<bool> stopping_{false};
    std::mutex m_;
    std::condition_variable cv_;

    const std::size_t cap_;
    std::vector<job> q_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;

    std::vector<std::thread> workers_;
};

// Awaitable that runs Fn on the blocking pool, then resumes on ctx's thread.
//
// IMPORTANT: Like your uring ops, this awaitable is embedded in the coroutine
// frame. Destroying the coroutine while the offload is in flight is UB.
// We "detect" this by tracking the awaitable in io_context's intrusive list
// and untracking on completion (the same fail-fast model you use for io_uring).
template <class Fn>
struct offload_op : operation_state {
    blocking_pool* pool = nullptr;
    Fn fn;

    using R = std::invoke_result_t<Fn>;
    std::conditional_t<std::is_void_v<R>, bool, std::optional<R>> value;
    std::exception_ptr ep;

    offload_op(io_context* c, blocking_pool* p, Fn&& f)
        : pool(p), fn(std::forward<Fn>(f)) {
        ctx = c;
    }

    bool await_ready() const noexcept { return false; }

    static void run(void* p) noexcept {
        auto* self = static_cast<offload_op*>(p);

        try {
            if constexpr (std::is_void_v<R>) {
                self->fn();
                self->value = true;
            } else {
                self->value.emplace(self->fn());
            }
        } catch (...) {
            self->ep = std::current_exception();
        }

        // Hand completion back to the owning io_context thread.
        // enqueue_external_done() coalesces wakes, so we don't spam MSG_RING.
        const bool need_wake = self->ctx->enqueue_external_done(self);
        if (need_wake) {
            detail::tls_waker.wake(self->ctx->ring_fd());
        }
    }

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        ctx->track(this);

        blocking_pool::job j{&run, this};
        if (!pool->try_submit(j)) {
            ctx->untrack(this);
            throw std::runtime_error("blocking_pool queue full");
        }
    }

    R await_resume() {
        if (ep) std::rethrow_exception(ep);
        if constexpr (std::is_void_v<R>) {
            return;
        } else {
            return std::move(*value);
        }
    }
};

template <class Fn>
auto offload(io_context& ctx, blocking_pool& pool, Fn&& fn) {
    return offload_op<std::decay_t<Fn>>{&ctx, &pool, std::forward<Fn>(fn)};
}

// -----------------------------------------------------------------------------
// Event (eventfd-based signaling)
// -----------------------------------------------------------------------------

/**
 * Simple one-shot or multi-shot event for coroutine signaling.
 *
 * Usage:
 *   event evt{&ctx};
 *
 *   // Waiter coroutine:
 *   auto result = co_await evt.wait().with_timeout(5s);
 *
 *   // Signaler (any thread):
 *   evt.signal();
 */
class event {
public:
    explicit event(io_context* ctx) : ctx_(ctx) {
        fd_ = eventfd(0, EFD_CLOEXEC);
        if (fd_ < 0) {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
    }

    ~event() { ::close(fd_); }

    event(const event&) = delete;
    event& operator=(const event&) = delete;

    struct wait_op : uring_op<wait_op> {
        int fd;
        uint64_t value{};

        wait_op(io_context* ctx, int fd) : uring_op(ctx), fd(fd) {}

        void prepare_sqe(io_uring_sqe* sqe) {
            io_uring_prep_read(sqe, fd, &value, sizeof(value), 0);
        }

        Result<uint64_t> await_resume() {
            if (res < 0) return std::unexpected(make_error_code(res));
            return value;
        }
    };

    wait_op wait() { return {ctx_, fd_}; }

    void signal(uint64_t count = 1) {
        [[maybe_unused]] auto r = ::write(fd_, &count, sizeof(count));
    }

    int fd() const { return fd_; }

private:
    io_context* ctx_;
    int fd_;
};

} // namespace aio
