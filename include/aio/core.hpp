#pragma once
#include <coroutine>
#include <expected>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

#include <liburing.h>

#include <sys/eventfd.h>
#include <sys/signalfd.h>

#include "aio/logger.hpp"
#include "pipe_pool.hpp"
#include "stats.hpp"
#include <openssl/err.h>

namespace aio
{
// forward declaration
class IoContext;

////////////////////////////////////////////////////////////////////////////////
// Standardized Error Handling
//
// Unifies the project on std::expected<T, std::error_code>.
// Allows usage of Result<int> or Result<> (defaults to void).
////////////////////////////////////////////////////////////////////////////////

template <typename T = void>
using Result = std::expected<T, std::error_code>;

inline std::unexpected<std::error_code> ErrorFromErrno(const int err) noexcept
{
    return std::unexpected(std::error_code(err, std::system_category()));
}

inline std::error_code MakeErrorCode(const int err) noexcept
{
    return std::error_code{err > 0 ? err : -err, std::system_category()};
}

namespace detail
{

struct openssl_category_t final : std::error_category
{
    const char* name() const noexcept override { return "openssl"; }

    std::string message(const int ev) const override
    {
        static_assert(sizeof(int) == 4, "This helper assumes 32-bit int");
        const auto bits = std::bit_cast<uint32_t>(ev);

        char buf[256];
        ERR_error_string_n(bits, buf, sizeof(buf));
        return buf;
    }
};

inline const std::error_category& openssl_category() noexcept
{
    static openssl_category_t cat;
    return cat;
}
}  // namespace detail

std::unexpected<std::error_code> ErrorFromOpenSSL() noexcept;

////////////////////////////////////////////////////////////////////////////////
// Task<T>/Task<void> - Minimal Coroutine Return Type
////////////////////////////////////////////////////////////////////////////////

template <typename T = void>
class [[nodiscard("You must co_await a Task or keep it alive")]] Task
{
public:
    struct promise_type
    {
        std::optional<T> value;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                if (h.promise().continuation != nullptr)
                {
                    return h.promise().continuation;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~Task() noexcept
    {
        if (handle_ != nullptr)
        {
            handle_.destroy();
        }
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool Done() const { return handle_ && handle_.done(); }

    T Result()
    {
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(*handle_.promise().value);
    }

    void resume()
    {
        if (handle_ != nullptr && !handle_.done())
        {
            handle_.resume();
        }
    }

    // Alias resume for clarity: Starts the task concurrently.
    // WARNING: You must still keep the 'task' object alive!
    void Start() { resume(); }

    // Awaitable interface
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) noexcept
    {
        handle_.promise().continuation = cont;
        return handle_;
    }

    T await_resume()
    {
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(*handle_.promise().value);
    }

private:
    handle_type handle_;
};

template <>
class [[nodiscard("You must co_await a Task or keep it alive")]] Task<void>
{
public:
    struct promise_type
    {
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter
        {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                if (h.promise().continuation)
                {
                    return h.promise().continuation;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        final_awaiter final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { exception = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~Task()
    {
        if (handle_ != nullptr)
        {
            handle_.destroy();
        }
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool Done() const { return handle_ && handle_.done(); }

    void Result() const
    {
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
    }

    void resume()
    {
        if (handle_ && !handle_.done())
        {
            handle_.resume();
        }
    }

    // Alias resume for clarity: Starts the task concurrently.
    // WARNING: You must still keep the 'task' object alive!
    void Start() { resume(); }

    // Awaitable interface
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) noexcept
    {
        handle_.promise().continuation = cont;
        return handle_;
    }

    void await_resume()
    {
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
    }

private:
    handle_type handle_;
};

////////////////////////////////////////////////////////////////////////////////
// Operation State (Intrusive Tracking)
////////////////////////////////////////////////////////////////////////////////

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
struct OperationState
{
    IoContext* ctx = nullptr;
    int32_t res = 0;
    std::coroutine_handle<> handle;

    // Intrusive doubly linked list pointers
    OperationState* next = nullptr;
    OperationState* prev = nullptr;
    bool tracked = false;

    OperationState() = default;

    // Move allowed only when not tracked
    OperationState(OperationState&& other) noexcept : ctx(other.ctx), res(other.res), handle(other.handle)
    {
        // Source must not be tracked
        if (other.tracked)
        {
            ALOG_ERROR(
                "[aio] FATAL: Attempted to move an OperationState that is currently tracked by "
                "IoContext.\n[aio]        This usually means a Task was moved while suspended on I/O.");
            std::terminate();
        }
        other.ctx = nullptr;
        other.handle = nullptr;
    }

    OperationState& operator=(OperationState&&) = delete;
    OperationState(const OperationState&) = delete;
    OperationState& operator=(const OperationState&) = delete;

    ~OperationState()
    {
        if (tracked == true)
        {
            // Coroutine destroyed while I/O pending → memory corruption risk
            // Terminate loudly rather than corrupt silently
            ALOG_ERROR(
                "[aio] FATAL: OperationState destroyed while still tracked by IoContext (I/O pending).\n[aio] "
                "       CAUSE: A Task was destroyed while suspended on an async operation.\n[aio]        FIX: "
                "  Ensure the Task is kept alive (e.g., in a TaskGroup) until it completes.");
            std::terminate();
        }
    }
};

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

////////////////////////////////////////////////////////////////////////////////
// Io Context
//
// Core IO construct for kio, a single threaded io_uring wrapper
////////////////////////////////////////////////////////////////////////////////

namespace detail
{
/// Reserved user_data value for internal eventfd wake
constexpr uint64_t WAKE_TAG = 1;
}  // namespace detail

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
            ALOG_ERROR("IoContext accessed from wrong thread!");
            std::terminate();
        }
#endif
    }

public:
    // user can pass their own io uring flag.
    // Note: using single issuer must ensure single issuer constraints are met.
    explicit IoContext(unsigned entries = 256);

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
    bool Notify() const noexcept;

    // -------------------------------------------------------------------------
    // Operation Tracking
    // -------------------------------------------------------------------------

    void Track(OperationState* op);
    void Untrack(OperationState* op);

    /**
     * Cancel all pending operations and drain completions.
     * Called automatically on destruction.
     *
     * IMPORTANT: This does NOT resume coroutines. Handles are dropped.
     */
    void CancelAllPending();

    Result<> RegisterFiles(std::span<const int> fds);

    template <typename T>
    void RunUntilDone(Task<T>&& t)
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
    bool EnqueueExternalDone(OperationState* op);
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

    void EnsureSqes(unsigned n);
    io_uring_sqe* GetSqe()
    {
        AssertOwnerThread();

        return io_uring_get_sqe(&ring_);
    }

    void WaitReady() const { ready_latch_.wait(); }
    void WaitStop() const { stopped_latch_.wait(); }
#if AIO_STATS
    IoContextStats& Stats() { return stats_; }
    const IoContextStats& Stats() const { return stats_; }
#endif

private:
    void DrainExternal(std::vector<std::coroutine_handle<>>& out);
    void DrainExternalWithoutResume();
    /**
     * Drain completions without resuming coroutines.
     * Used during destruction to safely untrack all pending ops.
     */
    // TODO: return error
    void DrainWithoutResume();
    void SubmitWakeRead();
    void Step();
    // process ready completions, return the number of completions processed
    // and if wake signal was seen for the caller to decide what to do about it
    std::pair<unsigned, bool> ProcessReadyCompletions();

    //
    // IoContext clss members
    //
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

#if AIO_STATS
    IoContextStats stats_{};
#endif
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
#if AIO_STATS
            AIO_STATS_INC(op.ctx->Stats(), timeouts);
#endif
            return decltype(r)(std::unexpected(std::make_error_code(std::errc::timed_out)));
        }
        return r;
    }
};

// Builder method to apply a timeout to an io operation
template <typename Rep, typename Period>
auto UringOp::WithTimeout(this auto&& self, std::chrono::duration<Rep, Period> dur)
    requires std::is_rvalue_reference_v<decltype(self)> && (!std::is_const_v<std::remove_reference_t<decltype(self)>>)
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

///////////////////////////////////////////////////////////////////
// Notifier
//////////////////////////////////////////////////////////////////

/// Thread-safe notification primitive.
///
/// Notifier allows a coroutine to wait for a signal from any thread.
/// It is designed for one-to-one signaling: a single Signal() call wakes up
/// exactly one waiting coroutine. This is NOT a broadcast mechanism (like
/// std::condition_variable::notify_all). If multiple coroutines wait on the
/// same Notifier, they will compete for signals.
///
/// It can be created without an IoContext, making it suitable for cross-thread
/// signaling where the IoContext might be created later or lives on a different
/// thread (e.g., inside a Worker).
///
/// @code
///   Notifier notifier;  // Created anywhere
///
///   // On worker thread:
///   co_await notifier.Wait(ctx);
///
///   // From any thread:
///   notifier.Signal();
/// @endcode
class Notifier
{
public:
    Notifier() : fd_(eventfd(0, EFD_CLOEXEC))
    {
        if (fd_ < 0)
        {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
    }

    ~Notifier()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    Notifier(Notifier&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    Notifier& operator=(Notifier&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    struct WaitOp : UringOp
    {
        int fd;
        uint64_t value{};

        WaitOp(IoContext* ctx, int f) : UringOp(ctx), fd(f) {}

        void PrepareSqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, &value, sizeof(value), 0); }

        /// Returns the number of signals that were pending
        Result<uint64_t> await_resume()
        {
            if (res < 0)
            {
                return std::unexpected(MakeErrorCode(res));
            }
            return value;
        }
    };

    /// Wait for one or more signals. Takes IoContext as a parameter.
    [[nodiscard]] WaitOp Wait(IoContext& ctx) { return {&ctx, fd_}; }

    /// Signal the notifier (thread-safe). Wakes up one pending Wait().
    void Signal(uint64_t count = 1) const { [[maybe_unused]] auto r = ::write(fd_, &count, sizeof(count)); }

    [[nodiscard]] int Fd() const { return fd_; }

private:
    int fd_;
};

//////////////////////////////////////////////////////////////////
// RingWaker - Cross-thread wake via eventfd
//////////////////////////////////////////////////////////////////

/**
 * Sends a wake signal to an io_context on another thread.
 *
 * Usage (e.g., for shutdown):
 * RingWaker waker;
 * for (auto& w : workers) {
 * w.ctx->stop();
 * waker.Wake(w.ctx->WakeFd());
 * }
 *
 * Lightweight wrapper around write(). Thread-safe.
 */
class RingWaker
{
public:
    RingWaker() = default;
    ~RingWaker() = default;

    /// Wake returns true if succeeded and false otherwise
    static bool Wake(const int wake_fd) noexcept
    {
        uint64_t val = 1;
        // Writing to the eventfd held by IoContext interrupts io_uring_wait_cqe
        // because IoContext keeps a persistent read on it.
        return ::write(wake_fd, &val, sizeof(val)) == sizeof(val);
    }

    static void Wake(const IoContext& ctx) noexcept { Wake(ctx.WakeFd()); }
};

/////////////////////////////////////////////////////////////////////////////
// Ip worker - convenience wrapper arround io context
/////////////////////////////////////////////////////////////////////////////
class Worker
{
public:
    explicit Worker(const int id = next_id_++, std::stop_token external_st = {})
        : id_(id), stop_token_(std::move(external_st))
    {
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    Worker(Worker&& other) noexcept
        : thread_(std::move(other.thread_)),
          wake_fd_(std::exchange(other.wake_fd_, -1)),
          id_(std::exchange(other.id_, -1)),
          tid_(std::exchange(other.tid_, 0)),
          stop_token_(std::move(other.stop_token_))
    {
    }

    Worker& operator=(Worker&& other) noexcept
    {
        if (this != &other)
        {
            Join();
            thread_ = std::move(other.thread_);
            wake_fd_ = std::exchange(other.wake_fd_, -1);
            id_ = std::exchange(other.id_, -1);
            tid_ = std::exchange(other.tid_, 0);
            stop_token_ = std::move(other.stop_token_);
        }
        return *this;
    }

    ~Worker() = default;

    /// @brief Spawns a thread, creates IoContext, and runs a user function.
    ///
    /// This is the low-level primitive for starting a worker. The user function
    /// receives the IoContext reference and is responsible for driving the event loop
    /// (e.g., calling ctx.Run() or ctx.RunUntilDone()).
    ///
    /// @tparam F Callable type (lambda or function object).
    /// @param func Function to execute on the worker thread. Signature: void(IoContext&).
    /// @param cpu_id CPU core to pin the thread to (-1 for no pinning).
    template <typename F>
    void Start(F&& func, int cpu_id = -1)
    {
        std::latch ready{1};
        std::atomic fd_out{-1};
        std::atomic<uint32_t> tid_out{0};

        auto ext_st = stop_token_;

        thread_ = std::jthread(
            [&ready, &fd_out, &tid_out, func = std::forward<F>(func), cpu_id, ext_st](std::stop_token st) mutable
            {
                if (cpu_id >= 0)
                {
                    PinToCpu(cpu_id);
                }

                const auto tid = static_cast<uint32_t>(::syscall(SYS_gettid));
                IoContext ctx(1024);

                auto stop_action = [&ctx]
                {
                    ctx.Stop();
                    (void)ctx.Notify();
                };

                std::stop_callback cb_internal(st, stop_action);

                if (ext_st.stop_possible())
                {
                    std::optional<std::stop_callback<decltype(stop_action)>> cb_external;
                    cb_external.emplace(ext_st, stop_action);
                }

                fd_out.store(ctx.WakeFd(), std::memory_order_relaxed);
                tid_out.store(tid, std::memory_order_relaxed);
                ready.count_down();

                func(ctx);
            });

        ready.wait();
        wake_fd_ = fd_out.load(std::memory_order_relaxed);
        tid_ = tid_out.load(std::memory_order_relaxed);
    }

    /// @brief Runs the IoContext loop with a periodic tick function.
    ///
    /// The loop runs until a stop is requested. The tick function is called
    /// after each batch of I/O completions.
    ///
    /// @tparam Tick Callable type for the tick function.
    /// @param tick Function to call periodically. Signature: bool() or void().
    /// @param cpu_id CPU core to pin the thread to (-1 for no pinning).
    template <typename Tick>
    void RunLoop(Tick&& tick, int cpu_id = -1)
    {
        Start(
            [tick = std::forward<Tick>(tick)](IoContext& ctx) mutable
            {
                ctx.Run(tick);
                ctx.CancelAllPending();
            },
            cpu_id);
    }

    /// @brief Runs the standard IoContext event loop.
    ///
    /// The loop runs indefinitely until a stop is requested via RequestStop()
    /// or the external stop token.
    ///
    /// @param cpu_id CPU core to pin the thread to (-1 for no pinning).
    void RunLoop(const int cpu_id = -1)
    {
        Start(
            [](IoContext& ctx)
            {
                ctx.Run();
                ctx.CancelAllPending();
            },
            cpu_id);
    }

    /// @brief Runs a single task to completion.
    ///
    /// The worker thread starts, executes the task generated by the factory,
    /// and exits once the task completes (co_returns).
    ///
    /// @tparam TaskFactory Callable returning a Task<T>.
    /// @param factory Function that creates the root task. Signature: Task<T>(IoContext&).
    /// @param cpu_id CPU core to pin the thread to (-1 for no pinning).
    template <typename TaskFactory>
    void RunTask(TaskFactory&& factory, int cpu_id = -1)
    {
        Start(
            [factory = std::forward<TaskFactory>(factory)](IoContext& ctx) mutable
            {
                auto task = factory(ctx);
                ctx.RunUntilDone(std::move(task));
                ctx.CancelAllPending();
            },
            cpu_id);
    }

    /// @brief Requests the worker thread to stop.
    ///
    /// This signals the IoContext to stop processing events and exit its run loop.
    void RequestStop() { thread_.request_stop(); }

    /// @brief Blocks until the worker thread finishes execution.
    void Join()
    {
        if (thread_.joinable())
            thread_.join();
    }

    /// @brief Checks if the worker thread is joinable.
    /// @return true if joinable, false otherwise.
    [[nodiscard]] bool Joinable() const { return thread_.joinable(); }

    /// @brief Wakes up the worker thread if it is sleeping in the event loop.
    /// @return true if the notification was sent successfully.
    bool Notify() const
    {
        if (wake_fd_ < 0)
            return false;
        constexpr uint64_t val = 1;
        return ::write(wake_fd_, &val, sizeof(val)) == sizeof(val);
    }

    [[nodiscard]] int Id() const { return id_; }
    [[nodiscard]] uint32_t ThreadId() const { return tid_; }
    [[nodiscard]] int WakeFd() const { return wake_fd_; }
    [[nodiscard]] std::stop_token StopToken() const { return stop_token_; }

private:
    static void PinToCpu(int cpu_id)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id % static_cast<int>(std::thread::hardware_concurrency()), &cpuset);

        if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
        {
            ALOG_INFO("Warning: Failed to pin to CPU {}: {}", cpu_id, std::strerror(rc));
        }
    }

    static inline std::atomic<int> next_id_{0};

    std::jthread thread_;
    int wake_fd_ = -1;
    int id_;
    uint32_t tid_ = 0;
    std::stop_token stop_token_;
};

}  // namespace aio