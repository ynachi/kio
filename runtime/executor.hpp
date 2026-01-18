#pragma once
////////////////////////////////////////////////////////////////////////////////
// executor.h - Minimal io_uring coroutine executor
//
// A simple, focused executor for storage engines and proxies.
// Requires: Linux 6.1+, liburing, C++23
////////////////////////////////////////////////////////////////////////////////

#include <chrono>
#include <concepts>
#include <coroutine>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <liburing.h>

#include "utilities/result.hpp"

namespace uring
{

class Executor;

template <typename T>
class Task;

namespace detail
{

/**
 * @brief Pipe pool
 */
class SplicePipePool
{
public:
    // RAII wrapper: returns FDs to the pool on destruction
    struct Lease
    {
        int read_fd = -1;
        int write_fd = -1;

        Lease() = default;
        Lease(int r, int w) : read_fd(r), write_fd(w) {}

        // Move-only implementation
        Lease(Lease&& other) noexcept
            : read_fd(std::exchange(other.read_fd, -1)),
              write_fd(std::exchange(other.write_fd, -1))
        {
        }

        Lease& operator=(Lease&& other) noexcept
        {
            if (this != &other)
            {
                recycle(); // Return current pipe to pool before taking new one
                read_fd = std::exchange(other.read_fd, -1);
                write_fd = std::exchange(other.write_fd, -1);
            }
            return *this;
        }

        ~Lease() { recycle(); }

        // Logic deferred to keep header clean
        void recycle();
    };

    static Result<Lease> acquire()
    {
        // 1. Try to pop from thread-local cache
        auto& pool = get_cache();
        if (!pool.empty())
        {
            auto [r, w] = pool.back();
            pool.pop_back();
            return Lease{r, w};
        }

        // 2. Cache miss: Create a new pipe
        int fds[2];
        // pipe2 saves us the extra fcntl calls for O_NONBLOCK
        if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0)
        {
            return error_from_errno(errno);
        }

        return Lease{fds[0], fds[1]};
    }

private:
    friend struct Lease;

    using PipePair = std::pair<int, int>;

    static std::vector<PipePair>& get_cache()
    {
        static thread_local std::vector<PipePair> cache_;
        return cache_;
    }

    static void return_to_pool(int r, int w)
    {
        auto& pool = get_cache();
        // Optional: Cap the cache size to prevent FD hoarding
        if (pool.size() < 64)
        {
            pool.emplace_back(r, w);
        }
        else
        {
            ::close(r);
            ::close(w);
        }
    }
};

// Out-of-line implementation to handle visibility
inline void SplicePipePool::Lease::recycle()
{
    if (read_fd != -1)
    {
        SplicePipePool::return_to_pool(read_fd, write_fd);
        read_fd = -1;
        write_fd = -1;
    }
}

// =====================================================
// Base promise type
// =====================================================


struct PromiseBase
{
    std::coroutine_handle<> continuation;
    std::exception_ptr exception;

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter
    {
        bool await_ready() noexcept { return false; }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
        {
            if (auto cont = h.promise().continuation)
                return cont;
            h.destroy();
            return std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() { exception = std::current_exception(); }
};

template <typename T>
struct Promise : PromiseBase
{
    std::optional<T> result;

    Task<T> get_return_object();

    // OPTIMIZED: Use overloads to handle lvalues and rvalues efficiently.
    // 1. fixes the `co_return {};` deduction issue (binds to T&&).
    // 2. avoids the extra move overhead of pass-by-value.

    void return_value(const T& value) { result = value; }

    void return_value(T&& value) { result = std::move(value); }
};

template <>
struct Promise<void> : PromiseBase
{
    Task<void> get_return_object();
    void return_void() {}
};

}  // namespace detail

template <typename T = void>
class Task
{
public:
    using promise_type = detail::Promise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

private:
    handle_type handle_;

public:
    explicit Task(handle_type h) : handle_(h) {}

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_)
                handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task()
    {
        if (handle_)
            handle_.destroy();
    }

    struct Awaiter
    {
        handle_type handle;

        bool await_ready() { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller)
        {
            handle.promise().continuation = caller;
            return handle;
        }

        T await_resume()
        {
            struct Guard
            {
                handle_type h;
                ~Guard()
                {
                    if (h)
                        h.destroy();
                }
            } guard{handle};
            if (handle.promise().exception)
                std::rethrow_exception(handle.promise().exception);
            if constexpr (!std::is_void_v<T>)
                return std::move(*handle.promise().result);
        }
    };

    Awaiter operator co_await() && { return Awaiter{std::exchange(handle_, nullptr)}; }

    handle_type handle() const { return handle_; }
    handle_type release() { return std::exchange(handle_, nullptr); }
};

namespace detail
{
template <typename T>
Task<T> Promise<T>::get_return_object()
{
    return Task<T>{std::coroutine_handle<Promise>::from_promise(*this)};
}

inline Task<> Promise<void>::get_return_object()
{
    return Task<>{std::coroutine_handle<Promise>::from_promise(*this)};
}
}  // namespace detail

////////////////////////////////////////////////////////////////////////////////
// BaseOp - Base class for io_uring operations
////////////////////////////////////////////////////////////////////////////////

struct BaseOp
{
    int32_t result = 0;
    uint32_t cqe_flags = 0;
    std::coroutine_handle<> handle;

    bool await_ready() const noexcept { return false; }
};

////////////////////////////////////////////////////////////////////////////////
// Executor - The event loop
////////////////////////////////////////////////////////////////////////////////

struct ExecutorConfig
{
    unsigned entries = 256;
    unsigned uring_flags = 0;
    unsigned sq_thread_idle_ms = 0;
};

class Executor
{
    io_uring ring_{};
    bool stopped_ = false;
    unsigned pending_ = 0;

public:
    using Config = ExecutorConfig;

    explicit Executor(const Config cfg = Config{})
    {
        io_uring_params params{};
        params.flags = cfg.uring_flags;
        params.sq_thread_idle = cfg.sq_thread_idle_ms;

        if (const int ret = io_uring_queue_init_params(cfg.entries, &ring_, &params); ret < 0)
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init");
    }

    ~Executor() { io_uring_queue_exit(&ring_); }

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    io_uring_sqe* get_sqe()
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);

        // Loop until we get a slot.
        // This handles backpressure if the kernel is slow to consume the SQ.
        while (!sqe)
        {
            // 1. Force submission of any pending items to the kernel.
            // This alerts the kernel to consume items from the SQ ring,
            // which frees up slots for us.
            io_uring_submit(&ring_);

            // 2. Try to get a fresh SQE again.
            sqe = io_uring_get_sqe(&ring_);
            if (sqe)
                break;

            // 3. Backoff.
            // If the ring is still full, the kernel is busy.
            // We yield the CPU to allow the kernel threads (or other processes)
            // to run and consume the submitted work.
            std::this_thread::yield();
        }

        ++pending_;
        return sqe;
    }

    int submit() { return io_uring_submit(&ring_); }

    void loop()
    {
        while (!stopped_ && pending_ > 0)
        {
            loop_once(true);
        }
    }

    void loop_forever()
    {
        while (!stopped_)
        {
            if (pending_ > 0)
                loop_once(true);
            else
                break;
        }
    }

    bool loop_once(const bool wait = true)
    {
        if (stopped_)
            return false;

        io_uring_submit(&ring_);

        if (pending_ == 0)
            return false;

        if (wait)
            io_uring_submit_and_wait(&ring_, 1);

        return process_completions() > 0;
    }

    template <typename T>
    void spawn(Task<T> task)
    {
        task.release().resume();
    }

    void stop() { stopped_ = true; }
    bool stopped() const { return stopped_; }
    unsigned pending() const { return pending_; }
    io_uring* ring() { return &ring_; }

private:
    unsigned process_completions()
    {
        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            auto* op = static_cast<BaseOp*>(io_uring_cqe_get_data(cqe));
            if (op)
            {
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
// Operations (Low Level - Raw Types)
////////////////////////////////////////////////////////////////////////////////

struct ReadOp : BaseOp
{
    Executor& exec;
    int fd;
    void* buf;
    unsigned len;
    uint64_t offset;

    ReadOp(Executor& ex, const int fd_, void* buf_, const unsigned len_, const uint64_t off)
        : exec(ex), fd(fd_), buf(buf_), len(len_), offset(off)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_read(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return result;
    }
};

struct WriteOp : BaseOp
{
    Executor& exec;
    int fd;
    const void* buf;
    unsigned len;
    uint64_t offset;

    WriteOp(Executor& ex, int fd_, const void* buf_, unsigned len_, uint64_t off)
        : exec(ex), fd(fd_), buf(buf_), len(len_), offset(off)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_write(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return result;
    }
};

struct FsyncOp : BaseOp
{
    Executor& exec;
    int fd;
    bool datasync;

    FsyncOp(Executor& ex, int fd_, bool datasync_ = false) : exec(ex), fd(fd_), datasync(datasync_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_fsync(sqe, fd, datasync ? IORING_FSYNC_DATASYNC : 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return {};
    }
};

struct AcceptOp : BaseOp
{
    Executor& exec;
    int listen_fd;
    sockaddr* addr;
    socklen_t* addrlen;

    AcceptOp(Executor& ex, int fd, sockaddr* addr_ = nullptr, socklen_t* len = nullptr)
        : exec(ex), listen_fd(fd), addr(addr_), addrlen(len)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return result;
    }
};

struct ConnectOp : BaseOp
{
    Executor& exec;
    int fd;
    const sockaddr* addr;
    socklen_t addrlen;

    ConnectOp(Executor& ex, int fd_, const sockaddr* addr_, socklen_t len)
        : exec(ex), fd(fd_), addr(addr_), addrlen(len)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_connect(sqe, fd, addr, addrlen);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return {};
    }
};

struct RecvOp : BaseOp
{
    Executor& exec;
    int fd;
    void* buf;
    unsigned len;
    int msg_flags;

    RecvOp(Executor& ex, int fd_, void* buf_, unsigned len_, int flags = 0)
        : exec(ex), fd(fd_), buf(buf_), len(len_), msg_flags(flags)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_recv(sqe, fd, buf, len, msg_flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return result;
    }
};

struct SendOp : BaseOp
{
    Executor& exec;
    int fd;
    const void* buf;
    unsigned len;
    int msg_flags;

    SendOp(Executor& ex, int fd_, const void* buf_, unsigned len_, int flags = 0)
        : exec(ex), fd(fd_), buf(buf_), len(len_), msg_flags(flags)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_send(sqe, fd, buf, len, msg_flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return result;
    }
};

struct CloseOp : BaseOp
{
    Executor& exec;
    int fd;

    CloseOp(Executor& ex, int fd_) : exec(ex), fd(fd_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_close(sqe, fd);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return {};
    }
};

struct TimeoutOp : BaseOp
{
    Executor& exec;
    __kernel_timespec ts;

    TimeoutOp(Executor& ex, __kernel_timespec t) : exec(ex), ts(t) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_timeout(sqe, &ts, 0, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result == -ETIME || result == 0)
            return {};
        return error_from_errno(-result);
    }
};

struct CancelOp : BaseOp
{
    Executor& exec;
    void* target;

    CancelOp(Executor& ex, void* user_data) : exec(ex), target(user_data) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_cancel(sqe, target, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result == 0 || result == -ENOENT || result == -EALREADY)
            return {};
        return error_from_errno(-result);
    }
};

struct CancelFdOp : BaseOp
{
    Executor& exec;
    int fd;

    CancelFdOp(Executor& ex, int fd_) : exec(ex), fd(fd_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_cancel_fd(sqe, fd, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
            return error_from_errno(-result);
        return result;
    }
};

struct ReadvOp : BaseOp
{
    Executor& exec;
    int fd;
    const iovec* iov;
    int iovcnt;
    uint64_t offset;

    ReadvOp(Executor& ex, int fd_, const iovec* iov_, int iovcnt_, uint64_t off)
        : exec(ex), fd(fd_), iov(iov_), iovcnt(iovcnt_), offset(off)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_readv(sqe, fd, iov, iovcnt, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }

        return result;
    }
};

struct WritevOp : BaseOp
{
    Executor& exec;
    int fd;
    const iovec* iov;
    int iovcnt;
    uint64_t offset;

    WritevOp(Executor& ex, int fd_, const iovec* iov_, int iovcnt_, uint64_t off)
        : exec(ex), fd(fd_), iov(iov_), iovcnt(iovcnt_), offset(off)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_writev(sqe, fd, iov, iovcnt, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }

        return result;
    }
};

struct PollOp : BaseOp
{
    Executor& exec;
    int fd;
    int events;

    PollOp(Executor& ex, int fd_, int events_) : exec(ex), fd(fd_), events(events_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_poll_add(sqe, fd, static_cast<unsigned>(events));
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }

        return result;
    }
};

struct SendmsgOp : BaseOp
{
    Executor& exec;
    int fd;
    const msghdr* msg;
    int flags;

    SendmsgOp(Executor& ex, int fd_, const msghdr* msg_, int flags_) : exec(ex), fd(fd_), msg(msg_), flags(flags_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_sendmsg(sqe, fd, msg, flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }
        return result;
    }
};

struct RecvmsgOp : BaseOp
{
    Executor& exec;
    int fd;
    msghdr* msg;
    int flags;

    RecvmsgOp(Executor& ex, int fd_, msghdr* msg_, int flags_) : exec(ex), fd(fd_), msg(msg_), flags(flags_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_recvmsg(sqe, fd, msg, flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }
        return result;
    }
};

struct OpenAtOp : BaseOp
{
    Executor& exec;
    int dirfd;
    std::filesystem::path path;
    int flags;
    mode_t mode;

    OpenAtOp(Executor& ex, int dirfd_, std::filesystem::path path_, int flags_, mode_t mode_)
        : exec(ex), dirfd(dirfd_), path(std::move(path_)), flags(flags_), mode(mode_)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_openat(sqe, dirfd, path.c_str(), flags, mode);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }
        return result;
    }
};

struct UnlinkAtOp : BaseOp
{
    Executor& exec;
    int dirfd;
    std::filesystem::path path;
    int flags;

    UnlinkAtOp(Executor& ex, int dirfd_, std::filesystem::path path_, int flags_)
        : exec(ex), dirfd(dirfd_), path(std::move(path_)), flags(flags_)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_unlinkat(sqe, dirfd, path.c_str(), flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }
        return {};
    }
};

struct FallocateOp : BaseOp
{
    Executor& exec;
    int fd;
    int mode;
    off_t offset;
    off_t len;

    FallocateOp(Executor& ex, int fd_, int mode_, off_t offset_, off_t len_)
        : exec(ex), fd(fd_), mode(mode_), offset(offset_), len(len_)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_fallocate(sqe, fd, mode, offset, len);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }
        return {};
    }
};

struct FtruncateOp : BaseOp
{
    Executor& exec;
    int fd;
    off_t length;

    FtruncateOp(Executor& ex, int fd_, off_t length_) : exec(ex), fd(fd_), length(length_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_ftruncate(sqe, fd, length);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }
        return {};
    }
};

struct SpliceOp : BaseOp
{
    Executor& exec;
    int fd_in;
    off_t off_in;
    int fd_out;
    off_t off_out;
    unsigned int len;
    unsigned int flags;

    SpliceOp(Executor& ex, int fd_in_, off_t off_in_, int fd_out_, off_t off_out_, unsigned int len_,
             unsigned int flags_ = 0)
        : exec(ex), fd_in(fd_in_), off_in(off_in_), fd_out(fd_out_), off_out(off_out_), len(len_), flags(flags_)
    {
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_splice(sqe, fd_in, off_in, fd_out, off_out, len, flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0)
        {
            return error_from_errno(-result);
        }
        return result;
    }
};

////////////////////////////////////////////////////////////////////////////////
// High Level API (Facade with Type Safety)
////////////////////////////////////////////////////////////////////////////////

// --- Concept Check for SocketAddress ---
// We don't include net.hpp, but we enforce the shape of the type.
template <typename T>
concept SocketAddressable = requires(const T& t) {
    { t.get() } -> std::convertible_to<const sockaddr*>;
    { t.addrlen } -> std::convertible_to<socklen_t>;
};

inline auto read(Executor& ex, int fd, std::span<char> buf, uint64_t off = 0)
{
    return ReadOp(ex, fd, buf.data(), buf.size_bytes(), off);
}

inline auto write(Executor& ex, int fd, std::span<const char> buf, uint64_t off = 0)
{
    return WriteOp(ex, fd, buf.data(), buf.size_bytes(), off);
}

inline auto fsync(Executor& ex, int fd, bool datasync = false)
{
    return FsyncOp(ex, fd, datasync);
}

inline auto accept(Executor& ex, int fd, sockaddr* addr = nullptr, socklen_t* len = nullptr)
{
    return AcceptOp(ex, fd, addr, len);
}

inline auto connect(Executor& ex, int fd, const sockaddr* addr, socklen_t len)
{
    return ConnectOp(ex, fd, addr, len);
}

template <SocketAddressable T>
auto connect(Executor& ex, int fd, const T& addr)
{
    return ConnectOp(ex, fd, addr.get(), addr.addrlen);
}

inline auto recv(Executor& ex, int fd, std::span<char> buf, int flags = 0)
{
    return RecvOp(ex, fd, buf.data(), buf.size_bytes(), flags);
}

inline auto send(Executor& ex, int fd, std::span<const char> buf, int flags = 0)
{
    return SendOp(ex, fd, buf.data(), buf.size_bytes(), flags);
}

inline auto close(Executor& ex, int fd)
{
    return CloseOp(ex, fd);
}

// Modern Timeout: Accepts std::chrono
template <typename Rep, typename Period>
auto timeout(Executor& ex, std::chrono::duration<Rep, Period> d)
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    __kernel_timespec ts{};
    ts.tv_sec = static_cast<int64_t>(ns / 1'000'000'000ULL);
    ts.tv_nsec = static_cast<long long>(ns % 1'000'000'000ULL);
    return TimeoutOp(ex, ts);
}

inline auto timeout_ms(Executor& ex, uint64_t ms)
{
    return timeout(ex, std::chrono::milliseconds(ms));
}

inline auto cancel(Executor& ex, void* target)
{
    return CancelOp(ex, target);
}

inline auto cancel_fd(Executor& ex, const int fd)
{
    return CancelFdOp(ex, fd);
}

inline auto readv(Executor& ex, int fd, const iovec* iov, int iovcnt, uint64_t offset = static_cast<uint64_t>(-1))
{
    return ReadvOp(ex, fd, iov, iovcnt, offset);
}

inline auto writev(Executor& ex, int fd, const iovec* iov, int iovcnt, uint64_t offset = static_cast<uint64_t>(-1))
{
    return WritevOp(ex, fd, iov, iovcnt, offset);
}

inline auto poll(Executor& ex, int fd, int events)
{
    return PollOp(ex, fd, events);
}

inline auto sendmsg(Executor& ex, int fd, const msghdr* msg, int flags = 0)
{
    return SendmsgOp(ex, fd, msg, flags);
}

inline auto recvmsg(Executor& ex, int fd, msghdr* msg, int flags = 0)
{
    return RecvmsgOp(ex, fd, msg, flags);
}

inline auto openat(Executor& ex, std::filesystem::path path, int flags, mode_t mode = 0)
{
    return OpenAtOp(ex, AT_FDCWD, std::move(path), flags, mode);
}

inline auto openat(Executor& ex, int dirfd, std::filesystem::path path, int flags, mode_t mode = 0)
{
    return OpenAtOp(ex, dirfd, std::move(path), flags, mode);
}

inline auto unlinkat(Executor& ex, std::filesystem::path path, int flags = 0)
{
    return UnlinkAtOp(ex, AT_FDCWD, std::move(path), flags);
}

inline auto unlinkat(Executor& ex, int dirfd, std::filesystem::path path, int flags = 0)
{
    return UnlinkAtOp(ex, dirfd, std::move(path), flags);
}

inline auto fallocate(Executor& ex, int fd, int mode, off_t size)
{
    return FallocateOp(ex, fd, mode, 0, size);
}

inline auto fallocate(Executor& ex, int fd, int mode, off_t offset, off_t len)
{
    return FallocateOp(ex, fd, mode, offset, len);
}

inline auto ftruncate(Executor& ex, int fd, off_t length)
{
    return FtruncateOp(ex, fd, length);
}

inline auto splice(Executor& ex, int fd_in, off_t off_in, int fd_out, off_t off_out, unsigned int len,
                   unsigned int flags = 0)
{
    return SpliceOp(ex, fd_in, off_in, fd_out, off_out, len, flags);
}

////////////////////////////////////////////////////////////////////////////////
// Helper Functions - Exact Read/Write
////////////////////////////////////////////////////////////////////////////////

// Read the exact number of bytes (loops until complete or EOF)
inline Task<Result<void>> read_exact(Executor& exec, int fd, std::span<char> buf)
{
    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (total_bytes_read < total_to_read)
    {
        auto res = co_await read(exec, fd, buf.subspan(total_bytes_read), static_cast<uint64_t>(-1));
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_read = *res;
        if (bytes_read == 0)
            co_return error_from_errno(EPIPE);  // EOF before reading complete

        total_bytes_read += static_cast<size_t>(bytes_read);
    }

    co_return {};
}

// Read exact number of bytes at offset (loops until complete or EOF)
inline Task<Result<void>> read_exact_at(Executor& exec, int fd, std::span<char> buf, uint64_t offset)
{
    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (total_bytes_read < total_to_read)
    {
        const uint64_t current_offset = offset + total_bytes_read;
        std::span<char> remaining_buf = buf.subspan(total_bytes_read);

        auto res = co_await read(exec, fd, remaining_buf, current_offset);
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_read = *res;
        if (bytes_read == 0)
            co_return error_from_errno(EPIPE);  // EOF before reading complete

        total_bytes_read += static_cast<size_t>(bytes_read);
    }

    co_return {};
}

// Write exact number of bytes (loops until complete)
inline Task<Result<void>> write_exact(Executor& exec, int fd, std::span<const char> buf)
{
    size_t total_bytes_written = 0;
    const size_t total_to_write = buf.size();

    while (total_bytes_written < total_to_write)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);

        auto res = co_await write(exec, fd, remaining_buf, static_cast<uint64_t>(-1));
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_written = *res;
        if (bytes_written == 0)
            co_return error_from_errno(EPIPE);  // EOF/error before writing complete

        total_bytes_written += static_cast<size_t>(bytes_written);
    }

    co_return {};
}

// Write the exact number of bytes at offset (loops until complete)
inline Task<Result<void>> write_exact_at(Executor& exec, int fd, std::span<const char> buf, uint64_t offset)
{
    size_t total_bytes_written = 0;
    const size_t total_to_write = buf.size();

    while (total_bytes_written < total_to_write)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
        const uint64_t current_offset = offset + total_bytes_written;

        auto res = co_await write(exec, fd, remaining_buf, current_offset);
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_written = *res;
        if (bytes_written == 0)
            co_return error_from_errno(EPIPE);  // EOF/error before writing complete

        total_bytes_written += static_cast<size_t>(bytes_written);
    }

    co_return {};
}

// Sendfile using splice with cached pipes
inline Task<Result<void>> sendfile(Executor& exec, int out_fd, int in_fd, off_t offset, size_t count)
{
    // 1. Acquire pipe from thread-local pool (Zero syscalls in hot path)
    auto pipe_lease = detail::SplicePipePool::acquire();
    if (!pipe_lease)
        co_return std::unexpected(pipe_lease.error());

    const int pipe_rd = pipe_lease->read_fd;
    const int pipe_wr = pipe_lease->write_fd;

    size_t remaining = count;
    off_t current_offset = offset;

    while (remaining > 0)
    {
        // Default pipe buffer on Linux is usually 64KB (16 pages)
        constexpr size_t chunk_size = 65536;
        const size_t to_splice = std::min(remaining, chunk_size);

        // 2. Splice from FILE -> PIPE
        auto res_in = co_await splice(exec, in_fd, current_offset, pipe_wr, static_cast<off_t>(-1),
                                      static_cast<unsigned int>(to_splice), 0);
        if (!res_in)
            co_return std::unexpected(res_in.error());

        const int bytes_in = *res_in;
        if (bytes_in == 0)
            co_return error_from_errno(EPIPE); // EOF unexpected here

        // 3. Splice from PIPE -> SOCKET/FILE
        // We must loop here because the pipe might be full or the socket might not accept all data at once
        auto pipe_remaining = static_cast<size_t>(bytes_in);
        while (pipe_remaining > 0)
        {
            auto res_out = co_await splice(exec, pipe_rd, static_cast<off_t>(-1), out_fd, static_cast<off_t>(-1),
                                           static_cast<unsigned int>(pipe_remaining), 0);
            if (!res_out)
                co_return std::unexpected(res_out.error());

            const int bytes_out = *res_out;
            if (bytes_out == 0)
                co_return error_from_errno(EPIPE);

            pipe_remaining -= static_cast<size_t>(bytes_out);
        }

        current_offset += bytes_in;
        remaining -= bytes_in;
    }

    // Lease destructor automatically returns the pipe to the pool here
    co_return {};
}
}  // namespace uring