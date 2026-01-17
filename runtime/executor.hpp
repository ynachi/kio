#pragma once
////////////////////////////////////////////////////////////////////////////////
// executor.h - Minimal io_uring coroutine executor
// 
// A simple, focused executor for storage engines and proxies.
// Requires: Linux 6.1+, liburing, C++23
////////////////////////////////////////////////////////////////////////////////

#include <liburing.h>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <cerrno>

namespace uring {

class Executor;

////////////////////////////////////////////////////////////////////////////////
// Error handling: std::expected<T, int> where error is errno
////////////////////////////////////////////////////////////////////////////////

template<typename T>
using Result = std::expected<T, int>;

// Convenience: create error result
inline std::unexpected<int> error(int e) { return std::unexpected(e); }

////////////////////////////////////////////////////////////////////////////////
// Task<T> - Lazy coroutine return type
////////////////////////////////////////////////////////////////////////////////

template<typename T>
class Task;

namespace detail {

struct PromiseBase {
    std::coroutine_handle<> continuation;
    std::exception_ptr exception;
    
    std::suspend_always initial_suspend() noexcept { return {}; }
    
    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        
        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
            if (auto cont = h.promise().continuation)
                return cont;
            // No continuation = top-level task. Self-destroy to avoid race.
            h.destroy();
            return std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() { exception = std::current_exception(); }
};

template<typename T>
struct Promise : PromiseBase {
    std::optional<T> result;

    Task<T> get_return_object();

    template<typename U>
    void return_value(U&& value) { result = std::forward<U>(value); }
};

template<>
struct Promise<void> : PromiseBase {
    Task<void> get_return_object();
    void return_void() {}
};

} // namespace detail

template<typename T = void>
class Task {
public:
    using promise_type = detail::Promise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

private:
    handle_type handle_;

public:
    explicit Task(handle_type h) : handle_(h) {}

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) handle_.destroy();
    }

    // Awaiter for co_await
    struct Awaiter {
        handle_type handle;

        bool await_ready() { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
            handle.promise().continuation = caller;
            return handle;
        }

        T await_resume() {
            // We own the handle now, ensure cleanup
            struct Guard {
                handle_type h;
                ~Guard() { if (h) h.destroy(); }
            } guard{handle};

            if (handle.promise().exception)
                std::rethrow_exception(handle.promise().exception);
            if constexpr (!std::is_void_v<T>)
                return std::move(*handle.promise().result);
        }
    };

    Awaiter operator co_await() && {
        return Awaiter{std::exchange(handle_, nullptr)};  // Transfer ownership to Awaiter
    }

    // For running as a top-level task
    handle_type handle() const { return handle_; }
    handle_type release() { return std::exchange(handle_, nullptr); }
};

namespace detail {
    template<typename T>
    Task<T> Promise<T>::get_return_object() {
        return Task<T>{std::coroutine_handle<Promise>::from_promise(*this)};
    }

    inline Task<> Promise<void>::get_return_object() {
        return Task<>{std::coroutine_handle<Promise>::from_promise(*this)};
    }
}

////////////////////////////////////////////////////////////////////////////////
// BaseOp - Base class for io_uring operations
////////////////////////////////////////////////////////////////////////////////

struct BaseOp {
    int32_t result = 0;
    uint32_t cqe_flags = 0;
    std::coroutine_handle<> handle;

    bool await_ready() const noexcept { return false; }

};

////////////////////////////////////////////////////////////////////////////////
// Executor - The event loop
////////////////////////////////////////////////////////////////////////////////

struct ExecutorConfig {
    unsigned entries = 256;
    // Raw io_uring flags - user's responsibility
    unsigned uring_flags = 0;
    // For IORING_SETUP_SQPOLL
    unsigned sq_thread_idle_ms = 0;
};

class Executor {
    io_uring ring_{};
    bool stopped_ = false;
    unsigned pending_ = 0;

public:
    using Config = ExecutorConfig;

    explicit Executor(const Config cfg = Config{}) {
        io_uring_params params{};
        params.flags = cfg.uring_flags;
        params.sq_thread_idle = cfg.sq_thread_idle_ms;

        if (const int ret = io_uring_queue_init_params(cfg.entries, &ring_, &params); ret < 0)
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init");
    }

    ~Executor() {
        io_uring_queue_exit(&ring_);
    }

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // Get an SQE, submitting if the ring is full
    io_uring_sqe* get_sqe() {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe)
            {
                throw std::runtime_error("SQ ring full even after submit");
            }
        }
        ++pending_;
        return sqe;
    }

    // Submit without waiting
    int submit() {
        return io_uring_submit(&ring_);
    }

    // Run until stopped or no more pending operations
    void run() {
        while (!stopped_ && pending_ > 0) {
            io_uring_submit_and_wait(&ring_, 1);
            process_completions();
        }
    }

    // Run one iteration
    // Returns: true if work was done, false if idle/stopped
    bool run_once(const bool wait = true) {
        if (stopped_)
        {
            return false;
        }

        // Submit any queued work
        io_uring_submit(&ring_);

        if (pending_ == 0) {
            // Nothing pending - if waiting, we'd block forever
            // Return false to let caller decide what to do
            return false;
        }

        if (wait == true) {
            io_uring_submit_and_wait(&ring_, 1);
        }

        return process_completions() > 0;
    }

    // Spawn a top-level task
    template<typename T>
    void spawn(Task<T> task) {
        task.release().resume();
    }

    void stop() { stopped_ = true; }
    bool stopped() const { return stopped_; }
    unsigned pending() const { return pending_; }

    io_uring* ring() { return &ring_; }

private:
    unsigned process_completions() {
        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring_, head, cqe) {
            auto* op = static_cast<BaseOp*>(io_uring_cqe_get_data(cqe));
            if (op) {
                op->result = cqe->res;
                op->cqe_flags = cqe->flags;
                --pending_;
                op->handle.resume();
            }
            ++count;
        }

        io_uring_cq_advance(&ring_, count);
        return count;
    }
};

////////////////////////////////////////////////////////////////////////////////
// Storage Operations
////////////////////////////////////////////////////////////////////////////////

struct ReadOp : BaseOp {
    Executor& exec;
    int fd;
    void* buf;
    unsigned len;
    uint64_t offset;

    ReadOp(Executor& ex, const int fd_, void* buf_, const unsigned len_, const uint64_t off = 0)
        : exec(ex), fd(fd_), buf(buf_), len(len_), offset(off) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_read(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume() {
        if (result < 0) return std::unexpected(-result);
        return result;
    }
};

struct WriteOp : BaseOp {
    Executor& exec;
    int fd;
    const void* buf;
    unsigned len;
    uint64_t offset;

    WriteOp(Executor& ex, int fd_, const void* buf_, unsigned len_, uint64_t off = 0)
        : exec(ex), fd(fd_), buf(buf_), len(len_), offset(off) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_write(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume() {
        if (result < 0) return std::unexpected(-result);
        return result;
    }
};

struct FsyncOp : BaseOp {
    Executor& exec;
    int fd;
    bool datasync;

    FsyncOp(Executor& ex, int fd_, bool datasync_ = false)
        : exec(ex), fd(fd_), datasync(datasync_) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_fsync(sqe, fd, datasync ? IORING_FSYNC_DATASYNC : 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume() {
        if (result < 0) return std::unexpected(-result);
        return {};
    }
};

////////////////////////////////////////////////////////////////////////////////
// Network Operations
////////////////////////////////////////////////////////////////////////////////

struct AcceptOp : BaseOp {
    Executor& exec;
    int listen_fd;
    sockaddr* addr;
    socklen_t* addrlen;

    AcceptOp(Executor& ex, int fd, sockaddr* addr_ = nullptr, socklen_t* len = nullptr)
        : exec(ex), listen_fd(fd), addr(addr_), addrlen(len) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume() {
        if (result < 0) return std::unexpected(-result);
        return result;
    }
};

struct ConnectOp : BaseOp {
    Executor& exec;
    int fd;
    const sockaddr* addr;
    socklen_t addrlen;

    ConnectOp(Executor& ex, int fd_, const sockaddr* addr_, socklen_t len)
        : exec(ex), fd(fd_), addr(addr_), addrlen(len) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_connect(sqe, fd, addr, addrlen);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume() {
        if (result < 0) return std::unexpected(-result);
        return {};
    }
};

struct RecvOp : BaseOp {
    Executor& exec;
    int fd;
    void* buf;
    unsigned len;
    int msg_flags;

    RecvOp(Executor& ex, int fd_, void* buf_, unsigned len_, int flags = 0)
        : exec(ex), fd(fd_), buf(buf_), len(len_), msg_flags(flags) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_recv(sqe, fd, buf, len, msg_flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume() {
        if (result < 0) return std::unexpected(-result);
        return result;
    }
};

struct SendOp : BaseOp {
    Executor& exec;
    int fd;
    const void* buf;
    unsigned len;
    int msg_flags;

    SendOp(Executor& ex, int fd_, const void* buf_, unsigned len_, int flags = 0)
        : exec(ex), fd(fd_), buf(buf_), len(len_), msg_flags(flags) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_send(sqe, fd, buf, len, msg_flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume() {
        if (result < 0) return std::unexpected(-result);
        return result;
    }
};

struct CloseOp : BaseOp {
    Executor& exec;
    int fd;

    CloseOp(Executor& ex, int fd_) : exec(ex), fd(fd_) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_close(sqe, fd);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume() {
        if (result < 0) return std::unexpected(-result);
        return {};
    }
};

////////////////////////////////////////////////////////////////////////////////
// Timing Operations
////////////////////////////////////////////////////////////////////////////////

struct TimeoutOp : BaseOp {
    Executor& exec;
    __kernel_timespec ts;

    TimeoutOp(Executor& ex, uint64_t ns) : exec(ex) {
        ts.tv_sec = static_cast<int64_t>(ns / 1'000'000'000ULL);
        ts.tv_nsec = static_cast<long long>(ns % 1'000'000'000ULL);
    }

    static TimeoutOp ms(Executor& ex, uint64_t millis) {
        return TimeoutOp(ex, millis * 1'000'000ULL);
    }

    static TimeoutOp sec(Executor& ex, uint64_t seconds) {
        return TimeoutOp(ex, seconds * 1'000'000'000ULL);
    }

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_timeout(sqe, &ts, 0, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume() {
        // -ETIME is expected for timeout expiry
        if (result == -ETIME || result == 0)
            return {};
        return std::unexpected(-result);
    }
};

////////////////////////////////////////////////////////////////////////////////
// Cancellation Operations
////////////////////////////////////////////////////////////////////////////////

struct CancelOp : BaseOp {
    Executor& exec;
    void* target;

    CancelOp(Executor& ex, void* user_data) : exec(ex), target(user_data) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_cancel(sqe, target, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume() {
        // -ENOENT = already completed, -EALREADY = cancel in progress
        if (result == 0 || result == -ENOENT || result == -EALREADY)
            return {};
        return std::unexpected(-result);
    }
};

struct CancelFdOp : BaseOp {
    Executor& exec;
    int fd;

    CancelFdOp(Executor& ex, int fd_) : exec(ex), fd(fd_) {}

    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_cancel_fd(sqe, fd, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume() {
        // Returns number of cancelled operations
        if (result < 0) return std::unexpected(-result);
        return result;
    }
};

////////////////////////////////////////////////////////////////////////////////
// Convenience Functions
////////////////////////////////////////////////////////////////////////////////

inline auto read(Executor& ex, int fd, void* buf, unsigned len, uint64_t off = 0) {
    return ReadOp(ex, fd, buf, len, off);
}

inline auto write(Executor& ex, int fd, const void* buf, unsigned len, uint64_t off = 0) {
    return WriteOp(ex, fd, buf, len, off);
}

inline auto fsync(Executor& ex, int fd, bool datasync = false) {
    return FsyncOp(ex, fd, datasync);
}

inline auto accept(Executor& ex, int fd, sockaddr* addr = nullptr, socklen_t* len = nullptr) {
    return AcceptOp(ex, fd, addr, len);
}

inline auto connect(Executor& ex, int fd, const sockaddr* addr, socklen_t len) {
    return ConnectOp(ex, fd, addr, len);
}

inline auto recv(Executor& ex, int fd, void* buf, unsigned len, int flags = 0) {
    return RecvOp(ex, fd, buf, len, flags);
}

inline auto send(Executor& ex, int fd, const void* buf, unsigned len, int flags = 0) {
    return SendOp(ex, fd, buf, len, flags);
}

inline auto close(Executor& ex, int fd) {
    return CloseOp(ex, fd);
}

inline auto timeout(Executor& ex, uint64_t ns) {
    return TimeoutOp(ex, ns);
}

inline auto timeout_ms(Executor& ex, uint64_t ms) {
    return TimeoutOp::ms(ex, ms);
}

inline auto cancel(Executor& ex, void* target) {
    return CancelOp(ex, target);
}

inline auto cancel_fd(Executor& ex, int fd) {
    return CancelFdOp(ex, fd);
}

} // namespace uring