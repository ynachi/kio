#pragma once
#include "kio/logger.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <csignal>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <latch>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <liburing.h>

#include <sys/eventfd.h>

#include "operation.hpp"
#include "pipe_pool.hpp"
#include "stats.hpp"
#include <openssl/err.h>

// =============================================================================
// Forward Declarations & Standard Type Registry
// =============================================================================

namespace kio
{
enum class ParseError : uint8_t;
}

// Register ParseError as an error_code enum (Must be in std namespace)
template <>
struct std::is_error_code_enum<kio::ParseError> : std::true_type
{
};

inline void PinToCpu(int cpu_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id % static_cast<int>(std::thread::hardware_concurrency()), &cpuset);

    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
    {
        ALOG_INFO("Warning: Failed to pin to CPU {}: {}", cpu_id, std::generic_category().message(rc));
    }
}

// =============================================================================
// Core KIO Definitions
// =============================================================================

namespace kio
{
// forward declaration
struct UringBackend;
struct MemoryBackend;
template <typename Backend>
class BasicIoContext;
using IoContext = BasicIoContext<UringBackend>;
using MemoryIoContext = BasicIoContext<MemoryBackend>;
template <typename T>
class TaskGroup;

////////////////////////////////////////////////////////////////////////////////
// Standardized Error Handling
//
// Unifies the project on std::expected<T, std::error_code>.
// Allows usage of Result<int> or Result<> (defaults to void).
////////////////////////////////////////////////////////////////////////////////

template <typename T = void>
using Result = std::expected<T, std::error_code>;

template <typename Backend>
concept IoBackend = requires(Backend backend, const Backend const_backend, unsigned entries, unsigned wait_nr,
                             std::span<const int> fds, OperationState* op) {
    { backend.Init(entries) } -> std::same_as<void>;
    { backend.Shutdown() } -> std::same_as<void>;
    { const_backend.Notify() } -> std::convertible_to<bool>;
    { backend.SubmitAndWait(wait_nr) } -> std::same_as<int>;
    { backend.CancelAllPending() } -> std::same_as<void>;
    { backend.RegisterFiles(fds) } -> std::same_as<Result<>>;
    { const_backend.WakeFd() } -> std::convertible_to<int>;
    { backend.TryMsgRing(const_backend, op) } -> std::convertible_to<bool>;
};

inline std::unexpected<std::error_code> ErrorFromErrno(const int err) noexcept
{
    return std::unexpected(std::error_code(err, std::system_category()));
}

inline std::error_code make_error_code(const int err) noexcept
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
        char buf[256];
        ERR_error_string_n(static_cast<unsigned long>(ev), buf, sizeof(buf));
        return buf;
    }
};

inline const std::error_category& openssl_category() noexcept
{
    static openssl_category_t cat;
    return cat;
}
}  // namespace detail

/**
 * Generic high-level parse errors.
 * These are protocol-agnostic, allowing different parsers to map
 * specific failures to these general categories.
 */
enum class ParseError : uint8_t
{
    Success = 0,
    /// Not enough data to finish frame
    Incomplete,
    /// Violation of protocol rules (bad characters, etc)
    InvalidProtocol,
    /// Data size exceeds limits
    Overflow,
    /// Logical failure in parser
    InternalError,
    /// invalid checksum
    Corrupted,
};

/**
 * Custom error category for Parsing
 */
class ParseErrorCategory : public std::error_category
{
public:
    const char* name() const noexcept override { return "kio::ParseError"; }

    std::string message(int ev) const override
    {
        switch (static_cast<ParseError>(ev))
        {
            case ParseError::Success:
                return "Success";
            case ParseError::Incomplete:
                return "Incomplete data (need more)";
            case ParseError::InvalidProtocol:
                return "Protocol violation / Invalid format";
            case ParseError::Overflow:
                return "Data exceeds buffer or protocol limits";
            case ParseError::InternalError:
                return "Internal parsing logic error";
            case ParseError::Corrupted:
                return "Data altered, invalid checksum, corrupted";
            default:
                return "Unknown parse error";
        }
    }
};

// Singleton instance of the category
inline const std::error_category& GetParseErrorCategory()
{
    static ParseErrorCategory instance;
    return instance;
}

// Overload make_error_code for ADL (must be snake_case)
inline std::error_code make_error_code(ParseError e)
{
    return {static_cast<int>(e), GetParseErrorCategory()};
}

inline std::unexpected<std::error_code> ErrorFromOpenSSL(unsigned long err) noexcept
{
    return std::unexpected(std::error_code(static_cast<int>(err), detail::openssl_category()));
}

std::unexpected<std::error_code> ErrorFromOpenSSL() noexcept;

////////////////////////////////////////////////////////////////////////////////
// Error Propagation Macros (Rust-style ? operator)
////////////////////////////////////////////////////////////////////////////////

namespace kio_try_internal
{
template <typename Exp>
auto unwrap_impl(Exp&& exp)
{
    using ValueT = std::decay_t<Exp>::value_type;
    if constexpr (!std::is_void_v<ValueT>)
    {
        return std::move(*std::forward<Exp>(exp));
    }
}
}  // namespace kio_try_internal

/**
 * @brief Sync Error Propagation.
 * Use inside normal functions. Returns `std::unexpected` on failure.
 * Usage: auto val = KIO_TRY(MyFunc());
 */
#define KIO_TRY(expr)                                \
    ({                                               \
        auto __res = (expr);                         \
        if (!__res)                                  \
        {                                            \
            return std::unexpected(__res.error());   \
        }                                            \
        ::kio::kio_try_internal::unwrap_impl(__res); \
    })

/**
 * @brief Async Error Propagation.
 * Use inside Coroutines. Co_returns `std::unexpected` on failure.
 * Usage: auto val = KIO_CO_TRY(co_await MyAsyncFunc());
 */
#define KIO_CO_TRY(expr)                              \
    ({                                                \
        auto __res = (expr);                          \
        if (!__res)                                   \
        {                                             \
            co_return std::unexpected(__res.error()); \
        }                                             \
        ::kio::kio_try_internal::unwrap_impl(__res);  \
    })

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
#ifndef NDEBUG
        if (!handle_ || !handle_.done())
        {
            ALOG_ERROR("[kio] Task::Result() called on incomplete or empty task");
            std::terminate();
        }
#endif
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(*handle_.promise().value);
    }

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
    template <typename Backend>
    friend class BasicIoContext;
    template <typename U>
    friend class TaskGroup;

    void resume()
    {
        if (handle_ != nullptr && !handle_.done())
        {
            handle_.resume();
        }
    }

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
#ifndef NDEBUG
        if (!handle_ || !handle_.done())
        {
            ALOG_ERROR("[kio] Task::Result() called on incomplete or empty task");
            std::terminate();
        }
#endif
        if (handle_.promise().exception)
        {
            std::rethrow_exception(handle_.promise().exception);
        }
    }

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
    template <typename Backend>
    friend class BasicIoContext;
    template <typename U>
    friend class TaskGroup;

    void resume()
    {
        if (handle_ != nullptr && !handle_.done())
        {
            handle_.resume();
        }
    }

    handle_type handle_;
};

template <typename Derived, typename Backend = UringBackend>
struct DispatchOp : OperationState
{
protected:
    explicit DispatchOp(BasicIoContext<Backend>* c) { ctx = c; }

    DispatchOp(DispatchOp&& other) noexcept : OperationState(std::move(other)) {}

public:
    BasicIoContext<Backend>& Context() { return *static_cast<BasicIoContext<Backend>*>(ctx); }
    const BasicIoContext<Backend>& Context() const { return *static_cast<const BasicIoContext<Backend>*>(ctx); }

    bool await_ready() const noexcept { return false; }

    void await_suspend(this auto& self, std::coroutine_handle<> h)
    {
        self.handle = h;
        auto* op = static_cast<OperationState*>(&self);
        auto& ctx = self.Context();
        ctx.Track(op);
        Submit(ctx.GetBackend(), ctx, self);
    }

    Result<size_t> await_resume()
    {
        if (res < 0)
        {
            return std::unexpected(make_error_code(res));
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

/**
 * @brief Helper RAII class to manage thread-local context tracking safely
 */
class ScopedIoContext
{
public:
    ScopedIoContext(void* ctx, const void* backend_tag);
    ~ScopedIoContext();
};

template <typename Backend>
const void* BackendTag() noexcept
{
    static const int tag = 0;
    return &tag;
}

struct UringBackend
{
    using NativeFileHandle = int;

    io_uring ring_{};
    int wake_fd_ = -1;
    uint64_t wake_buffer_ = 0;

    void Init(unsigned entries);
    void Shutdown() noexcept;
    bool Notify() const noexcept;
    int SubmitAndWait(unsigned wait_nr);
    bool TryMsgRing(const UringBackend& target, OperationState* op);
    void CancelAllPending();
    Result<> RegisterFiles(std::span<const int> fds);

    io_uring* Ring() { return &ring_; }
    const io_uring* Ring() const { return &ring_; }
    int RingFd() const { return ring_.ring_fd; }
    int WakeFd() const { return wake_fd_; }

    void EnsureSqes(unsigned n);
    io_uring_sqe* GetSqe();
    void SubmitWakeRead();
    void FlushAfterWake();

    template <typename OnCompletion>
    bool DrainWithoutResume(OnCompletion&& on_completion)
    {
        io_uring_cqe* cqe = nullptr;
        int ret = 0;
        do
        {
            ret = io_uring_wait_cqe(&ring_, &cqe);
        } while (ret == -EINTR);

        if (ret < 0)
        {
            return false;
        }

        const auto user_data = io_uring_cqe_get_data64(cqe);
        on_completion(user_data);
        io_uring_cqe_seen(&ring_, cqe);
        return true;
    }

    template <typename OnCompletion>
    std::pair<unsigned, bool> ProcessReadyCompletions(OnCompletion&& on_completion)
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
                SubmitWakeRead();
                continue;
            }

            on_completion(reinterpret_cast<OperationState*>(static_cast<uintptr_t>(user_data)), cqe->res);
        }

        io_uring_cq_advance(&ring_, count);
        return {count, saw_wake};
    }
};

inline io_uring_sqe* PrepareSqe(UringBackend& backend)
{
    backend.EnsureSqes(1);
    return backend.GetSqe();
}

inline std::pair<io_uring_sqe*, io_uring_sqe*> PrepareLinkedTimeoutSqes(UringBackend& backend, __kernel_timespec& ts)
{
    backend.EnsureSqes(2);

    auto* sqe_op = backend.GetSqe();
    auto* sqe_timer = backend.GetSqe();
    sqe_timer->flags |= IOSQE_CQE_SKIP_SUCCESS;
    io_uring_prep_link_timeout(sqe_timer, &ts, 0);
    io_uring_sqe_set_data(sqe_timer, nullptr);
    return {sqe_op, sqe_timer};
}

struct MemoryBackend
{
    using NativeFileHandle = uint64_t;
    using clock = std::chrono::steady_clock;

    enum class TimeMode : uint8_t
    {
        AutoAdvance,
        ManualAdvance,
    };

    struct Config
    {
        TimeMode time_mode = TimeMode::AutoAdvance;
        clock::duration start_time{};
        size_t default_max_read_bytes = 0;
        size_t default_max_write_bytes = 0;
    };

    struct IoFault
    {
        int error = 0;
        size_t max_bytes = 0;
    };

    struct FileState
    {
        std::vector<std::byte> data;
        bool fsynced = false;
    };

    struct OpenFileState
    {
        std::shared_ptr<FileState> file;
        int flags = 0;
    };

    struct TimerEntry
    {
        clock::time_point due;
        OperationState* op = nullptr;
        bool operator>(const TimerEntry& o) const noexcept { return due > o.due; }
    };

    Config config_{};
    std::deque<OperationState*> ready_;
    // Min-heap: soonest deadline at top — O(log n) insert, O(1) peek.
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>> timers_;
    std::unordered_map<std::string, std::shared_ptr<FileState>> files_;
    std::unordered_map<NativeFileHandle, OpenFileState> open_files_;
    std::deque<IoFault> open_faults_;
    std::deque<IoFault> read_faults_;
    std::deque<IoFault> write_faults_;
    std::deque<IoFault> close_faults_;
    std::deque<IoFault> fsync_faults_;
    NativeFileHandle next_handle_ = 1;
    int wake_fd_ = -1;
    uint64_t wake_buffer_ = 0;
    std::atomic<int64_t> now_ns_{0};

    MemoryBackend() = default;
    explicit MemoryBackend(Config config) : config_(config), now_ns_(config.start_time.count()) {}
    MemoryBackend(MemoryBackend&& other) noexcept
        : config_(other.config_),
          ready_(std::move(other.ready_)),
          timers_(std::move(other.timers_)),
          files_(std::move(other.files_)),
          open_files_(std::move(other.open_files_)),
          open_faults_(std::move(other.open_faults_)),
          read_faults_(std::move(other.read_faults_)),
          write_faults_(std::move(other.write_faults_)),
          close_faults_(std::move(other.close_faults_)),
          fsync_faults_(std::move(other.fsync_faults_)),
          next_handle_(other.next_handle_),
          wake_fd_(other.wake_fd_),
          wake_buffer_(other.wake_buffer_),
          now_ns_(other.now_ns_.load(std::memory_order_acquire))
    {
        other.next_handle_ = 1;
        other.wake_fd_ = -1;
        other.wake_buffer_ = 0;
        other.now_ns_.store(0, std::memory_order_release);
    }
    MemoryBackend& operator=(MemoryBackend&& other) noexcept
    {
        if (this != &other)
        {
            config_ = other.config_;
            ready_ = std::move(other.ready_);
            timers_ = std::move(other.timers_);
            files_ = std::move(other.files_);
            open_files_ = std::move(other.open_files_);
            open_faults_ = std::move(other.open_faults_);
            read_faults_ = std::move(other.read_faults_);
            write_faults_ = std::move(other.write_faults_);
            close_faults_ = std::move(other.close_faults_);
            fsync_faults_ = std::move(other.fsync_faults_);
            next_handle_ = other.next_handle_;
            wake_fd_ = other.wake_fd_;
            wake_buffer_ = other.wake_buffer_;
            now_ns_.store(other.now_ns_.load(std::memory_order_acquire), std::memory_order_release);

            other.next_handle_ = 1;
            other.wake_fd_ = -1;
            other.wake_buffer_ = 0;
            other.now_ns_.store(0, std::memory_order_release);
        }
        return *this;
    }
    MemoryBackend(const MemoryBackend&) = delete;
    MemoryBackend& operator=(const MemoryBackend&) = delete;

    void Init(unsigned);
    void Shutdown() noexcept;
    bool Notify() const noexcept;
    int SubmitAndWait(unsigned);
    bool TryMsgRing(const MemoryBackend&, OperationState*) { return false; }
    void CancelAllPending();
    Result<> RegisterFiles(std::span<const int>) { return {}; }
    int WakeFd() const { return wake_fd_; }
    void SubmitWakeRead() {}
    void FlushAfterWake() {}
    void AddTimer(OperationState* op, clock::time_point due);
    void Complete(OperationState* op, int32_t res);
    clock::time_point Now() const noexcept
    {
        return clock::time_point(clock::duration(now_ns_.load(std::memory_order_acquire)));
    }
    void AdvanceTime(clock::duration delta) noexcept { now_ns_.fetch_add(delta.count(), std::memory_order_acq_rel); }
    void AdvanceTo(clock::time_point tp) noexcept
    {
        auto desired = tp.time_since_epoch().count();
        auto current = now_ns_.load(std::memory_order_acquire);
        while (desired > current &&
               !now_ns_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire))
        {
        }
    }
    void QueueOpenError(int error) { open_faults_.push_back(IoFault{.error = error}); }
    void QueueReadError(int error) { read_faults_.push_back(IoFault{.error = error}); }
    void QueueWriteError(int error) { write_faults_.push_back(IoFault{.error = error}); }
    void QueueCloseError(int error) { close_faults_.push_back(IoFault{.error = error}); }
    void QueueFsyncError(int error) { fsync_faults_.push_back(IoFault{.error = error}); }
    void QueueReadPartial(size_t max_bytes) { read_faults_.push_back(IoFault{.max_bytes = max_bytes}); }
    void QueueWritePartial(size_t max_bytes) { write_faults_.push_back(IoFault{.max_bytes = max_bytes}); }
    void SetDefaultMaxReadBytes(size_t max_bytes) noexcept { config_.default_max_read_bytes = max_bytes; }
    void SetDefaultMaxWriteBytes(size_t max_bytes) noexcept { config_.default_max_write_bytes = max_bytes; }
    Result<NativeFileHandle> OpenFile(const std::filesystem::path& path, int flags, mode_t mode);
    Result<size_t> ReadFile(NativeFileHandle handle, std::span<std::byte> buffer, uint64_t offset);
    Result<size_t> WriteFile(NativeFileHandle handle, std::span<const std::byte> buffer, uint64_t offset);
    Result<void> CloseFile(NativeFileHandle handle);
    Result<void> FsyncFile(NativeFileHandle handle);

    template <typename OnCompletion>
    bool DrainWithoutResume(OnCompletion&& on_completion)
    {
        if (ready_.empty())
        {
            return false;
        }

        auto* op = ready_.front();
        ready_.pop_front();

        on_completion(reinterpret_cast<uint64_t>(op));
        return true;
    }

    template <typename OnCompletion>
    std::pair<unsigned, bool> ProcessReadyCompletions(OnCompletion&& on_completion)
    {
        std::deque<OperationState*> ready;
        ready.swap(ready_);

        for (auto* op : ready)
        {
            on_completion(op, op->res);
        }

        return {static_cast<unsigned>(ready.size()), false};
    }
};

template <typename Backend>
class BasicIoContext
{
    static_assert(IoBackend<Backend>, "BasicIoContext requires an IoBackend-compatible backend");
    //
    // IoContext clss members
    //
    Backend backend_{};
    std::vector<std::coroutine_handle<>> ready_;
    OperationState* pending_head_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::once_flag pipe_pool_flag_;
    std::latch ready_latch_{1};
    std::latch stopped_latch_{1};

    // External completions (lock-free MPSC intrusive stack)
    std::atomic<OperationState*> ext_submission_head_{nullptr};
    std::atomic<bool> ext_hint_ = false;  // fast-path hint
    std::atomic_bool shutdown_requested_{false};

    // Lazy pipe pool for sendfile operations
    std::optional<PipePool> pipe_pool_;

#if AIO_STATS
    IoContextStats stats_{};
#endif
    std::thread::id owner_thread_ = std::this_thread::get_id();

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
    explicit BasicIoContext(unsigned entries = 16800);
    BasicIoContext(Backend backend, unsigned entries);

    ~BasicIoContext() noexcept;

    // --- Static Access ---
    /// Returns the IoContext running on the current thread, or nullptr.
    static BasicIoContext* Current() noexcept;

    BasicIoContext(const BasicIoContext&) = delete;
    BasicIoContext& operator=(const BasicIoContext&) = delete;

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
    Backend& GetBackend() { return backend_; }
    const Backend& GetBackend() const { return backend_; }

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
    void CancelAllPending(OpCancelReason reason = OpCancelReason::None);

    Result<> RegisterFiles(std::span<const int> fds);

    /// @brief Returns true if shutdown has been requested
    /// @note Coroutines can check this to perform graceful cleanup.
    [[nodiscard]] bool IsShuttingDown() const { return shutdown_requested_.load(std::memory_order_acquire); }

    /// @brief  Request graceful shutdown, coroutines could check IsShuttingDown and exit cleanly
    void RequestShutdown() noexcept
    {
        shutdown_requested_.store(true, std::memory_order_release);
        Stop();
        (void)Notify();
    }

    template <typename T>
    void RunUntilDone(Task<T>&& t)
    {
        AssertOwnerThread();
        ScopedIoContext scope(this, BackendTag<Backend>());

        stop_requested_.store(false, std::memory_order_relaxed);

        t.resume();
        while (!IsShuttingDown() && (!t.Done() || pending_head_ != nullptr ||
                                     ext_submission_head_.load(std::memory_order_acquire) != nullptr))
        {
            Step();
        }

        stopped_latch_.count_down();
    }

    void Run()
    {
        AssertOwnerThread();
        ScopedIoContext scope(this, BackendTag<Backend>());

        stop_requested_.store(false, std::memory_order_relaxed);

        while (!IsShuttingDown())
        {
            Step();
        }

        stopped_latch_.count_down();
    }

    template <typename Tick>
    void Run(Tick&& tick)
    {
        AssertOwnerThread();
        ScopedIoContext scope(this, BackendTag<Backend>());

        stop_requested_.store(false, std::memory_order_relaxed);

        while (!IsShuttingDown())
        {
            Step();
            tick();
        }

        stopped_latch_.count_down();
    }

    /// Request stop and wait for complete shutdown if needed
    void Stop(const bool wait = false)
    {
        bool was_already_stopped = stop_requested_.exchange(true, std::memory_order_release);

        // NOTIFY: Wake up the thread if it's sleeping in io_uring_wait_cqe
        // We only need to notify if we actually changed the state,
        // but notifying safely is cheap enough to do unconditionally.
        if (!was_already_stopped)
        {
            (void)Notify();
        }

        if (wait)
        {
            // If we are the worker thread, we CANNOT wait for ourselves to finish!
            if (std::this_thread::get_id() != owner_thread_)
            {
                WaitStop();
            }
            else
            {
                ALOG_ERROR("We can not wait for ourself to stop");
            }
        }
    }

    // ---------------------------------------------------------------------
    // Cross-thread completion injection (for blocking pool offload, etc.)
    // ---------------------------------------------------------------------

    /**
     * Enqueue a completed operation that must be resumed on this io_context
     * thread. Thread-safe and lock-free.
     * Handles notification automatically if the thread is sleeping.
     */
    void SubmitExternal(OperationState* op);

    /**
     * @brief Tries to send an IORING_OP_MSG_RING to target. Returns true on success.
     * Fails if current thread has no ring.
     */
    bool TryMsgRing(const BasicIoContext& target, OperationState* op);

    // -------------------------------------------------------------------------
    // Low-level Access
    // -------------------------------------------------------------------------

    /**
     * Get the pipe pool for sendfile operations.
     * Created lazily on first access.
     */
    PipePool& GetPipePool()
    {
        std::call_once(pipe_pool_flag_, [this] { pipe_pool_.emplace(4); });
        return *pipe_pool_;
    }

    int WakeFd() const { return backend_.WakeFd(); }

    void WaitReady() const { ready_latch_.wait(); }
    void WaitStop() const { stopped_latch_.wait(); }
#if AIO_STATS
    IoContextStats& Stats() { return stats_; }
    const IoContextStats& Stats() const { return stats_; }
#endif

    /// @brief Schedules the current coroutine to resume on this IoContext.
    /// @return Awaitable that suspends the coroutine and resumes it on this context's thread.
    /// @note Thread-safe to call from any thread.
    [[nodiscard]] auto Schedule();

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
};

// -----------------------------------------------------------------------------
// Signal Handling
// -----------------------------------------------------------------------------

/**
 * RAII signal handler using eventfd.
 *
 * Installs signal handlers that write to an eventfd, allowing io_uring to
 * wait for signals without requiring signals to be blocked beforehand.
 *
 * Note: Only one SignalSet instance should be active at a time.
 */
class SignalSet
{
public:
    SignalSet(std::initializer_list<int> sigs);
    ~SignalSet();

    SignalSet(const SignalSet&) = delete;
    SignalSet& operator=(const SignalSet&) = delete;

    int fd() const { return fd_; }

private:
    static void SignalHandler(int sig);

    int fd_ = -1;
    std::vector<std::pair<int, struct sigaction>> old_actions_;

    static inline std::atomic<SignalSet*> instance_{nullptr};
};

struct WaitSignalOp : DispatchOp<WaitSignalOp>
{
    int fd;
    uint64_t signo{};

    WaitSignalOp(IoContext& ctx, const int signal_fd) : DispatchOp(&ctx), fd(signal_fd) {}

    Result<int> await_resume();
};

inline void Submit(UringBackend& backend, IoContext&, WaitSignalOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_read(sqe, op.fd, &op.signo, sizeof(op.signo), 0);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, WaitSignalOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_read(sqe_op, op.fd, &op.signo, sizeof(op.signo), 0);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

inline WaitSignalOp AsyncWaitSignal(IoContext& ctx, int signal_fd)
{
    return WaitSignalOp(ctx, signal_fd);
}

inline WaitSignalOp AsyncWaitSignal(IoContext& ctx, const SignalSet& set)
{
    return WaitSignalOp(ctx, set.fd());
}

// -----------------------------------------------------------------------------
// Timeout Wrapper
// -----------------------------------------------------------------------------

/// Inefficient when we have million of timers
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
        auto& ctx = op.Context();
        ctx.Track(&op);
        SubmitWithTimeout(ctx.GetBackend(), ctx, op, ts);
    }

    auto await_resume()
    {
        auto r = op.await_resume();

        // Translate ECANCELED to timed_out for clarity
        if (!r && r.error().value() == ECANCELED)
        {
            // If the reason is None, it implies io_uring cancelled it via the
            // linked timeout mechanism, because CancelAllPending() would have
            // set it to something else (e.g., ContextShutdown).
            if (op.cancel_reason == OpCancelReason::None || op.cancel_reason == OpCancelReason::Timeout)
            {
#if AIO_STATS
                AIO_STATS_INC(op.Context().Stats(), timeouts);
#endif
                return decltype(r)(std::unexpected(std::make_error_code(std::errc::timed_out)));
            }
            return r;
        }

        return r;
    }
};

template <typename Derived, typename Backend>
template <typename Rep, typename Period>
auto DispatchOp<Derived, Backend>::WithTimeout(this auto&& self, std::chrono::duration<Rep, Period> dur)
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

    struct WaitOp : DispatchOp<WaitOp>
    {
        int fd;
        uint64_t value{};

        WaitOp(IoContext& ctx, int f) : DispatchOp(&ctx), fd(f) {}

        /// Returns the number of signals that were pending
        Result<uint64_t> await_resume()
        {
            if (res < 0)
            {
                return std::unexpected(make_error_code(res));
            }
            return value;
        }
    };

    friend inline void Submit(UringBackend& backend, IoContext&, WaitOp& op)
    {
        auto* sqe = PrepareSqe(backend);
        io_uring_prep_read(sqe, op.fd, &op.value, sizeof(op.value), 0);
        io_uring_sqe_set_data(sqe, &op);
    }

    friend inline void SubmitWithTimeout(UringBackend& backend, IoContext&, WaitOp& op, __kernel_timespec& ts)
    {
        auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
        io_uring_prep_read(sqe_op, op.fd, &op.value, sizeof(op.value), 0);
        sqe_op->flags |= IOSQE_IO_LINK;
        io_uring_sqe_set_data(sqe_op, &op);
    }

    /// Wait for one or more signals. Takes IoContext as a parameter.
    [[nodiscard]] WaitOp Wait(IoContext& ctx) { return {ctx, fd_}; }

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
        constexpr uint64_t val = 1;
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
                // TODO: use param
                IoContext ctx(16800);

                auto stop_action = [&ctx] { ctx.RequestShutdown(); };

                std::stop_callback cb_internal(st, stop_action);
                std::optional<std::stop_callback<decltype(stop_action)>> cb_external;  // NOLINT
                if (ext_st.stop_possible())
                {
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
    static inline std::atomic<int> next_id_{0};

    std::jthread thread_;
    int wake_fd_ = -1;
    int id_;
    uint32_t tid_ = 0;
    std::stop_token stop_token_;
};

////////////////////////////////////////////////////////////////////////////////
// Formatting Support
////////////////////////////////////////////////////////////////////////////////

// Helper wrapper to format std::error_code nicely
struct FmtErr
{
    const std::error_code& code;
};
}  // namespace kio

// =============================================================================
// std::formatter Specializations
// =============================================================================

template <>
struct std::formatter<kio::ParseError>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(kio::ParseError e, std::format_context& ctx) const
    {
        return std::format_to(ctx.out(), "ParseError: {} ({})", kio::make_error_code(e).message(), static_cast<int>(e));
    }
};

template <>
struct std::formatter<kio::FmtErr>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(kio::FmtErr w, std::format_context& ctx) const
    {
        // Output: "Category: Message (Value)"
        return std::format_to(ctx.out(), "{}: {} ({})", w.code.category().name(), w.code.message(), w.code.value());
    }
};

// =============================================================================
// Scheduling / Context Switch Support
// =============================================================================

namespace kio
{
/**
 * Operation to migrate a coroutine to a specific IoContext.
 *
 * This awaits internally, suspending the coroutine on the current thread,
 * and resuming it on the target IoContext thread.
 */
template <typename Backend>
struct ScheduleOp : OperationState
{
    BasicIoContext<Backend>* target;

    explicit ScheduleOp(BasicIoContext<Backend>* t) : target(t) { ctx = t; }

    bool await_ready() const noexcept
    {
        // If we are already on the target context, don't suspend.
        return BasicIoContext<Backend>::Current() == target;
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        // Try Zero-Syscall path: IORING_OP_MSG_RING
        if (auto* current = BasicIoContext<Backend>::Current(); current && current->TryMsgRing(*target, this))
        {
            return;
        }

        // Fallback path: Lock-free intrusive stack + batched eventfd
        target->SubmitExternal(this);
    }

    void await_resume() const noexcept {}
};

/**
 * @brief Low-level primitive to schedule resumption on this context.
 * @note Prefer using the `SwitchTo(ctx)` helper for better readability.
 */
template <typename Backend>
inline auto BasicIoContext<Backend>::Schedule()
{
    return ScheduleOp<Backend>(this);
}

/**
 * @brief Helper to switch execution to the target IoContext.
 *
 * Usage:
 * co_await SwitchTo(target_ctx);
 *
 * If the coroutine is already running on target_ctx, this is a no-op (no suspension).
 * Otherwise, it migrates the coroutine to the target thread.
 */
template <typename Backend>
[[nodiscard]]
inline auto SwitchTo(BasicIoContext<Backend>& target)
{
    return target.Schedule();
}

/**
 * Helper to spawn a fire-and-forget task on a target IoContext.
 *
 * Usage:
 * task_group.Spawn(CoSpawn(target_ctx, []() {
 * // This runs on target_ctx
 * }));
 */
template <typename Fun>
Task<> CoSpawn(IoContext& ctx, Fun f)
{
    // Suspend here and resume on the target context
    co_await SwitchTo(ctx);

    // Execute the user function (which should probably return an awaitable)
    if constexpr (std::is_void_v<std::invoke_result_t<Fun>>)
    {
        f();
    }
    else
    {
        co_await f();
    }
}
}  // namespace kio
