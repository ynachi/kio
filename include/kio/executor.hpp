#pragma once
////////////////////////////////////////////////////////////////////////////////
// executor.h - Minimal io_uring coroutine executor
//
// A simple, focused executor for storage engines and proxies.
// Requires: Linux 6.1+, liburing, C++23
////////////////////////////////////////////////////////////////////////////////

#include <coroutine>
#include <expected>
#include <filesystem>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <liburing.h>

#include "kio/result.hpp"

namespace kio
{

// Forward declarations for public types
template <typename T> class Task;

namespace detail
{

// -----------------------------------------------------------------------------
// Internal: Pipe Pool
// -----------------------------------------------------------------------------
class SplicePipePool
{
public:
    struct Lease
    {
        int read_fd = -1;
        int write_fd = -1;

        Lease() = default;
        Lease(const int r, const int w) : read_fd(r), write_fd(w) {}

        Lease(Lease&& other) noexcept
            : read_fd(std::exchange(other.read_fd, -1)),
              write_fd(std::exchange(other.write_fd, -1))
        {
        }

        Lease& operator=(Lease&& other) noexcept
        {
            if (this != &other)
            {
                recycle();
                read_fd = std::exchange(other.read_fd, -1);
                write_fd = std::exchange(other.write_fd, -1);
            }
            return *this;
        }

        ~Lease() noexcept { recycle(); }

        void recycle() noexcept;
    };

    static Result<Lease> acquire()
    {
        if (auto& pool = get_cache(); !pool.empty())
        {
            auto [r, w] = pool.back();
            pool.pop_back();
            return Lease{r, w};
        }

        int fds[2];
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
        thread_local std::vector<PipePair> cache_;
        return cache_;
    }

    static void return_to_pool(int r, int w)
    {
        if (auto& pool = get_cache(); pool.size() < 64)
            pool.emplace_back(r, w);
        else
        {
            ::close(r);
            ::close(w);
        }
    }
};

// -----------------------------------------------------------------------------
// Internal: Coroutine Promise
// -----------------------------------------------------------------------------
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
            {
                return cont;
            }
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
    void return_value(const T& value) { result = value; }
    void return_value(T&& value) { result = std::move(value); }
};

template <>
struct Promise<void> : PromiseBase
{
    Task<void> get_return_object();
    void return_void() {}
};

} // namespace detail

// -----------------------------------------------------------------------------
// Public: Task
// -----------------------------------------------------------------------------
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

    ~Task() noexcept
    {
        if (handle_)
        {
            handle_.destroy();
        }
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
            struct Guard { handle_type h; ~Guard() { if (h) h.destroy(); } } guard{handle};
            if (handle.promise().exception)
            {
                std::rethrow_exception(handle.promise().exception);
            }
            if constexpr (!std::is_void_v<T>)
            {
                return std::move(*handle.promise().result);
            }
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
    return kio::Task<T>{std::coroutine_handle<Promise>::from_promise(*this)};
}

inline Task<> Promise<void>::get_return_object()
{
    return Task{std::coroutine_handle<Promise>::from_promise(*this)};
}

// -----------------------------------------------------------------------------
// Internal: Executor & Operations
// Moved to detail namespace. Methods are public but hidden from user API.
// -----------------------------------------------------------------------------

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
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init");
        }
    }

    ~Executor() noexcept { io_uring_queue_exit(&ring_); }

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    void stop() { stopped_ = true; }
    bool stopped() const { return stopped_; }
    unsigned pending() const { return pending_; }
    int ring_fd() const { return ring_.ring_fd; }

    io_uring_sqe* get_sqe()
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        int retries = 0;
        while (!sqe)
        {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (sqe) break;

            // If we've spun too long, force wait for a completion
            if (++retries > 100)
            {
                io_uring_submit_and_wait(&ring_, 1);
                retries = 0;
            }
            std::this_thread::yield();
        }
        ++pending_;
        return sqe;
    }

    // variant of get_sqe cqes tasks not handled by this executor
    io_uring_sqe* get_sqe_untracked()
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        while (!sqe)
        {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (sqe) break;
            std::this_thread::yield();
        }
        return sqe;
    }

    int submit() { return io_uring_submit(&ring_); }

    void msg_ring_wake(const int target_ring_fd, const unsigned int len = 1, const __u64 data = 0) noexcept
    {
        io_uring_sqe* sqe = get_sqe_untracked();
        io_uring_prep_msg_ring(sqe, target_ring_fd, len, data, 0);
        io_uring_sqe_set_data(sqe, nullptr);
        sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
        (void)io_uring_submit(&ring_);
    }

    void loop()
    {
        while (!stopped_ && pending_ > 0) loop_once(true);
    }

    void loop_forever()
    {
        while (!stopped_)
        {
            if (pending_ > 0) loop_once(true);
            else break;
        }
    }

    bool loop_once(const bool wait = true)
    {
        if (stopped_) return false;
        io_uring_submit(&ring_);
        if (pending_ == 0) return false;
        if (wait) io_uring_submit_and_wait(&ring_, 1);
        return process_completions() > 0;
    }

    template <typename T>
    void spawn(Task<T> task)
    {
        task.release().resume();
    }

private:
    // BaseOp must be forward declared or defined before use
    unsigned process_completions();
};

// Base class for all ops
struct BaseOp
{
    int32_t result = 0;
    uint32_t cqe_flags = 0;
    std::coroutine_handle<> handle;
    bool await_ready() const noexcept { return false; }
};

inline unsigned Executor::process_completions()
{
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        if (auto* op = static_cast<BaseOp*>(io_uring_cqe_get_data(cqe)))
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

// -----------------------------------------------------------------------------
// Internal Operations
// All moved to detail. They take detail::Executor reference.
// -----------------------------------------------------------------------------

struct ReadOp : BaseOp
{
    Executor& exec;
    int fd;
    void* buf;
    unsigned len;
    uint64_t offset;

    ReadOp(Executor& ex, const int fd_, void* buf_, const unsigned len_, const uint64_t off)
        : exec(ex), fd(fd_), buf(buf_), len(len_), offset(off)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_read(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return result;
    }

    // Prevent moves (operations are meant to be temporaries)
    ReadOp(ReadOp&&) = delete;
    ReadOp& operator=(ReadOp&&) = delete;
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
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_write(sqe, fd, buf, len, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return result;
    }

    // Prevent moves (operations are meant to be temporaries)
    WriteOp(WriteOp&&) = delete;
    WriteOp& operator=(WriteOp&&) = delete;
};

struct FsyncOp : BaseOp
{
    Executor& exec;
    int fd;
    bool datasync;

    FsyncOp(Executor& ex, int fd_, bool datasync_ = false)
        : exec(ex), fd(fd_), datasync(datasync_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_fsync(sqe, fd, datasync ? IORING_FSYNC_DATASYNC : 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return {};
    }

    FsyncOp(FsyncOp&&) = delete;
    FsyncOp& operator=(FsyncOp&&) = delete;
};

struct AcceptOp : BaseOp
{
    Executor& exec;
    int listen_fd;
    sockaddr* addr;
    socklen_t* addrlen;

    AcceptOp(Executor& ex, int fd, sockaddr* addr_ = nullptr, socklen_t* len = nullptr)
        : exec(ex), listen_fd(fd), addr(addr_), addrlen(len)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return result;
    }

    AcceptOp(AcceptOp&&) = delete;
    AcceptOp& operator=(AcceptOp&&) = delete;
};

struct ConnectOp : BaseOp
{
    Executor& exec;
    int fd;
    const sockaddr* addr;
    socklen_t addrlen;

    ConnectOp(ConnectOp&&) = delete;
    ConnectOp& operator=(ConnectOp&&) = delete;

    ConnectOp(Executor& ex, int fd_, const sockaddr* addr_, socklen_t len)
        : exec(ex), fd(fd_), addr(addr_), addrlen(len)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_connect(sqe, fd, addr, addrlen);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
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

    RecvOp(RecvOp&&) = delete;
    RecvOp& operator=(RecvOp&&) = delete;

    RecvOp(Executor& ex, int fd_, void* buf_, unsigned len_, int flags = 0)
        : exec(ex), fd(fd_), buf(buf_), len(len_), msg_flags(flags)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_recv(sqe, fd, buf, len, msg_flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
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

    SendOp(SendOp&&) = delete;
    SendOp& operator=(SendOp&&) = delete;

    SendOp(Executor& ex, int fd_, const void* buf_, unsigned len_, int flags = 0)
        : exec(ex), fd(fd_), buf(buf_), len(len_), msg_flags(flags)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_send(sqe, fd, buf, len, msg_flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return result;
    }
};

struct CloseOp : BaseOp
{
    Executor& exec;
    int fd;

    CloseOp(CloseOp&&) = delete;
    CloseOp& operator=(CloseOp&&) = delete;

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
        if (result < 0) return error_from_errno(-result);
        return {};
    }
};

struct TimeoutOp : BaseOp
{
    Executor& exec;
    __kernel_timespec ts;

    TimeoutOp(TimeoutOp&&) = delete;
    TimeoutOp& operator=(TimeoutOp&&) = delete;

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
        if (result == -ETIME || result == 0) return {};
        return error_from_errno(-result);
    }
};

struct CancelOp : BaseOp
{
    Executor& exec;
    void* target;

    CancelOp(CancelOp&&) = delete;
    CancelOp& operator=(CancelOp&&) = delete;

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
        if (result == 0 || result == -ENOENT || result == -EALREADY) return {};
        return error_from_errno(-result);
    }
};

struct CancelFdOp : BaseOp
{
    Executor& exec;
    int fd;
    int flags;

    CancelFdOp(CancelFdOp&&) = delete;
    CancelFdOp& operator=(CancelFdOp&&) = delete;

    // use IORING_ASYNC_CANCEL_ALL to cancel all operations related to the FD
    CancelFdOp(Executor& ex, int fd_, int flags_=0) : exec(ex), fd(fd_), flags(flags_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_cancel_fd(sqe, fd, flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
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

    ReadvOp(ReadvOp&&) = delete;
    ReadvOp& operator=(ReadvOp&&) = delete;

    ReadvOp(Executor& ex, int fd_, const iovec* iov_, int iovcnt_, uint64_t off)
        : exec(ex), fd(fd_), iov(iov_), iovcnt(iovcnt_), offset(off)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_readv(sqe, fd, iov, iovcnt, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
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

    WritevOp(WritevOp&&) = delete;
    WritevOp& operator=(WritevOp&&) = delete;

    WritevOp(Executor& ex, int fd_, const iovec* iov_, int iovcnt_, uint64_t off)
        : exec(ex), fd(fd_), iov(iov_), iovcnt(iovcnt_), offset(off)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_writev(sqe, fd, iov, iovcnt, offset);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return result;
    }
};

struct PollOp : BaseOp
{
    Executor& exec;
    int fd;
    int events;

    PollOp(PollOp&&) = delete;
    PollOp& operator=(PollOp&&) = delete;

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
        if (result < 0) return error_from_errno(-result);
        return result;
    }
};

struct SendmsgOp : BaseOp
{
    Executor& exec;
    int fd;
    const msghdr* msg;
    int flags;

    SendmsgOp(SendmsgOp&&) = delete;
    SendmsgOp& operator=(SendmsgOp&&) = delete;

    SendmsgOp(Executor& ex, int fd_, const msghdr* msg_, int flags_)
        : exec(ex), fd(fd_), msg(msg_), flags(flags_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_sendmsg(sqe, fd, msg, flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return result;
    }
};

struct RecvmsgOp : BaseOp
{
    Executor& exec;
    int fd;
    msghdr* msg;
    int flags;

    RecvmsgOp(RecvmsgOp&&) = delete;
    RecvmsgOp& operator=(RecvmsgOp&&) = delete;

    RecvmsgOp(Executor& ex, int fd_, msghdr* msg_, int flags_)
        : exec(ex), fd(fd_), msg(msg_), flags(flags_) {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_recvmsg(sqe, fd, msg, flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
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

    OpenAtOp(OpenAtOp&&) = delete;
    OpenAtOp& operator=(OpenAtOp&&) = delete;

    OpenAtOp(Executor& ex, int dirfd_, std::filesystem::path path_, int flags_, mode_t mode_)
        : exec(ex), dirfd(dirfd_), path(std::move(path_)), flags(flags_), mode(mode_)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_openat(sqe, dirfd, path.c_str(), flags, mode);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return result;
    }
};

struct UnlinkAtOp : BaseOp
{
    Executor& exec;
    int dirfd;
    std::filesystem::path path;
    int flags;

    UnlinkAtOp(UnlinkAtOp&&) = delete;
    UnlinkAtOp& operator=(UnlinkAtOp&&) = delete;

    UnlinkAtOp(Executor& ex, int dirfd_, std::filesystem::path path_, int flags_)
        : exec(ex), dirfd(dirfd_), path(std::move(path_)), flags(flags_)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_unlinkat(sqe, dirfd, path.c_str(), flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
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

    FallocateOp(FallocateOp&&) = delete;
    FallocateOp& operator=(FallocateOp&&) = delete;

    FallocateOp(Executor& ex, int fd_, int mode_, off_t offset_, off_t len_)
        : exec(ex), fd(fd_), mode(mode_), offset(offset_), len(len_)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_fallocate(sqe, fd, mode, offset, len);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<void> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return {};
    }
};

struct FtruncateOp : BaseOp
{
    Executor& exec;
    int fd;
    off_t length;

    FtruncateOp(FtruncateOp&&) = delete;
    FtruncateOp& operator=(FtruncateOp&&) = delete;

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
        if (result < 0) return error_from_errno(-result);
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

    SpliceOp(SpliceOp&&) = delete;
    SpliceOp& operator=(SpliceOp&&) = delete;

    SpliceOp(Executor& ex, int fd_in_, off_t off_in_, int fd_out_, off_t off_out_, unsigned int len_,
             unsigned int flags_ = 0)
        : exec(ex), fd_in(fd_in_), off_in(off_in_), fd_out(fd_out_), off_out(off_out_), len(len_), flags(flags_)
    {}

    void await_suspend(std::coroutine_handle<> h)
    {
        handle = h;
        io_uring_sqe* sqe = exec.get_sqe();
        io_uring_prep_splice(sqe, fd_in, off_in, fd_out, off_out, len, flags);
        io_uring_sqe_set_data(sqe, this);
    }

    Result<int> await_resume()
    {
        if (result < 0) return error_from_errno(-result);
        return result;
    }
};

}  // namespace detail
}  // namespace kio