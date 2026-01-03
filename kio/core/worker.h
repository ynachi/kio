//
// Created by Yao ACHI on 17/10/2025.
//

#ifndef KIO_WORKER_H
#define KIO_WORKER_H

#include "coro.h"
#include "errors.h"
#include "kio/net/net.h"
#include "kio/net/socket.h"
#include "kio/sync/mpsc_queue.h"
#include "safe_completion.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <latch>
#include <span>
#include <thread>
#include <tuple>

#include <fcntl.h>
#include <liburing.h>

namespace kio::io
{

class Worker;

namespace internal
{
struct WorkerAccess
{
    static void Post(Worker& worker, std::coroutine_handle<> h);
    static io_uring& GetRing(Worker& worker) noexcept;
};
}  // namespace internal

struct alignas(std::hardware_destructive_interference_size) WorkerStats
{
    uint64_t bytes_read_total = 0;
    uint64_t bytes_written_total = 0;
    uint64_t read_ops_total = 0;
    uint64_t write_ops_total = 0;
    uint64_t connections_accepted_total = 0;
    uint64_t open_ops_total = 0;
    uint64_t connect_ops_total = 0;
    uint64_t coroutines_pool_resize_total = 0;
    uint64_t active_coroutines = 0;
    uint64_t io_errors_total = 0;
};

struct WorkerConfig
{
    size_t uring_queue_depth{1024};
    uint32_t max_idle_wait_us{2000};
    size_t tcp_backlog{128};
    uint8_t uring_submit_timeout_ms{100};
    int uring_default_flags = 0;
    size_t max_op_slots{1024 * 1024};
    size_t task_queue_capacity{1024};
    uint32_t heartbeat_interval_us{100};
    uint32_t busy_wait_us{20};
    size_t task_batch_size{64};

    void Check() const
    {
        if (uring_queue_depth == 0 || tcp_backlog == 0)
        {
            throw std::invalid_argument("Invalid worker config");
        }
        if (task_queue_capacity == 0 || (task_queue_capacity & (task_queue_capacity - 1)) != 0)
        {
            throw std::invalid_argument("task_queue_capacity must be a power of 2.");
        }
        if (task_batch_size == 0)
        {
            throw std::invalid_argument("task_batch_size must be > 0");
        }
    }
};

/**
 * IoUringAwaitable - Using SafeIoCompletion for robust cancellation
 */
template <typename Prep, typename... Args>
struct IoUringAwaitable
{
    Worker& worker;
    Prep io_uring_prep;
    using OnSuccess = void (*)(Worker&, int);
    OnSuccess on_success;
    std::tuple<Args...> io_args;

    // Heap-allocated, ref-counted completion state
    SafeIoCompletion* completion_state{nullptr};

    bool await_ready() const noexcept { return false; }  // NOLINT

    bool await_suspend(std::coroutine_handle<> h);  // NOLINT

    [[nodiscard]] Result<int> await_resume() noexcept  // NOLINT
    {
        // Get result
        int res = completion_state->result;

        // We are done with the object on the User side. Release User ref.
        completion_state->Release();
        completion_state = nullptr;  // Clear to prevent dtor from abandoning

        if (res < 0)
        {
            return std::unexpected(Error::FromErrno(-res));
        }
        if (on_success != nullptr)
        {
            on_success(worker, res);
        }
        return res;
    }

    explicit IoUringAwaitable(Worker& worker, Prep prep, OnSuccess on_success, Args... args)
        : worker(worker), io_uring_prep(std::move(prep)), on_success(on_success), io_args(args...)
    {
    }

    ~IoUringAwaitable()
    {
        // If completion_state is still set, it means await_resume() was NEVER called.
        // This implies the coroutine is being destroyed while suspended (Cancellation).
        if (completion_state != nullptr)
        {
            completion_state->Abandon();
        }
    }

    IoUringAwaitable(const IoUringAwaitable&) = delete;
    IoUringAwaitable& operator=(const IoUringAwaitable&) = delete;
    IoUringAwaitable(IoUringAwaitable&&) = delete;
    IoUringAwaitable& operator=(IoUringAwaitable&&) = delete;
};

template <typename Prep, typename... Args>
auto MakeUringAwaitable(Worker& worker, Prep&& prep, void (*on_success)(Worker&, int), Args&&... args)
{
    return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(worker, std::forward<Prep>(prep), on_success,
                                                                       std::forward<Args>(args)...);
}

template <typename Prep, typename... Args>
auto MakeUringAwaitable(Worker& worker, Prep&& prep, Args&&... args)
{
    return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(worker, std::forward<Prep>(prep), nullptr,
                                                                       std::forward<Args>(args)...);
}

class Worker
{
    friend struct internal::WorkerAccess;
    template <typename, typename...>
    friend struct IoUringAwaitable;

    io_uring ring_{};
    WorkerConfig config_;
    size_t id_{0};
    std::atomic<std::thread::id> thread_id_;
    MPSCQueue<std::coroutine_handle<>> task_queue_;

    std::latch init_latch_{1};
    std::latch shutdown_latch_{1};
    std::stop_source stop_source_;
    std::stop_token stop_token_;
    std::atomic<bool> stopped_{false};
    WorkerStats stats_{};
    std::function<void(Worker&)> worker_init_callback_;

    // Accept dynamic wait time for adaptive heartbeat
    int SubmitSqesWait(uint32_t wait_us);

    static void StatIncRead(Worker& w, const int res)
    {
        w.stats_.bytes_read_total += res;
        w.stats_.read_ops_total++;
    }
    static void StatIncWrite(Worker& w, const int res)
    {
        w.stats_.bytes_written_total += res;
        w.stats_.write_ops_total++;
    }
    static void StatIncAccept(Worker& w, int) { w.stats_.connections_accepted_total++; }
    static void StatIncConnect(Worker& w, int) { w.stats_.connect_ops_total++; }
    static void StatIncOpen(Worker& w, int) { w.stats_.open_ops_total++; }

    static void CheckKernelVersion();
    void CheckSyscallReturn(int ret);
    unsigned ProcessCompletions();
    void DrainCompletions();

    void Post(std::coroutine_handle<> h);
    io_uring& GetRing() noexcept { return ring_; }
    static void HandleCqe(io_uring_cqe* cqe);
    void Loop();

public:
    [[nodiscard]] bool IsOnWorkerThread() const;
    void SignalInitComplete() { init_latch_.count_down(); }
    void SignalShutdownComplete() { shutdown_latch_.count_down(); }
    [[nodiscard]] std::stop_token GetStopToken() const { return stop_source_.get_token(); }
    [[nodiscard]] size_t GetId() const noexcept { return id_; }
    void LoopForever();
    void WaitReady() const { init_latch_.wait(); }
    void WaitShutdown() const { shutdown_latch_.wait(); }
    [[nodiscard]] bool RequestStop();
    void Initialize();
    [[nodiscard]] bool IsRunning() const noexcept { return !stopped_.load(std::memory_order_acquire); }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;
    Worker(size_t id, const WorkerConfig& config, std::function<void(Worker&)> worker_init_callback = {});
    ~Worker();

    [[nodiscard]] const WorkerStats& GetStats() { return stats_; }

    [[nodiscard]] auto AsyncAccept(int server_fd, sockaddr* addr, socklen_t* addrlen)
    {
        auto prep = [](io_uring_sqe* sqe, int fd, sockaddr* a, socklen_t* al, int flags)
        { io_uring_prep_accept(sqe, fd, a, al, flags); };
        return MakeUringAwaitable(*this, prep, &Worker::StatIncAccept, server_fd, addr, addrlen, 0);
    }

    [[nodiscard]] auto AsyncAccept(const net::Socket& socket, net::SocketAddress& addr)
    {
        return AsyncAccept(socket.get(), reinterpret_cast<sockaddr*>(&addr.addr), &addr.addrlen);
    }

    [[nodiscard]] auto AsyncReadAt(int client_fd, std::span<char> buf, uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, char* b, const unsigned len, const uint64_t off)
        { io_uring_prep_read(sqe, fd, b, len, off); };
        return MakeUringAwaitable(*this, prep, &Worker::StatIncRead, client_fd, buf.data(),
                                  static_cast<unsigned>(buf.size()), offset);
    }

    [[nodiscard]] auto AsyncRead(const int client_fd, std::span<char> buf)
    {
        return AsyncReadAt(client_fd, buf, static_cast<uint64_t>(-1));
    }

    [[nodiscard]] auto AsyncWriteAt(const int fd, std::span<const char> buf, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const char* b, const unsigned len, const uint64_t off)
        { io_uring_prep_write(sqe, fd, b, len, off); };
        return MakeUringAwaitable(*this, prep, &Worker::StatIncWrite, fd, buf.data(), static_cast<unsigned>(buf.size()),
                                  offset);
    }

    [[nodiscard]] auto AsyncReadv(int client_fd, const iovec* iov, int iovcnt, uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, int fd, const iovec* iov, int iovcnt, uint64_t off)
        { io_uring_prep_readv(sqe, fd, iov, iovcnt, off); };
        return MakeUringAwaitable(*this, prep, &Worker::StatIncRead, client_fd, iov, iovcnt, offset);
    }

    [[nodiscard]] auto AsyncWrite(int client_fd, std::span<const char> buf)
    {
        return AsyncWriteAt(client_fd, buf, static_cast<uint64_t>(-1));
    }

    [[nodiscard]] auto AsyncWritev(int client_fd, const iovec* iov, int iovcnt, uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const iovec* iov, int iovcnt, const uint64_t off)
        { io_uring_prep_writev(sqe, fd, iov, iovcnt, off); };
        return MakeUringAwaitable(*this, prep, &Worker::StatIncWrite, client_fd, iov, iovcnt, offset);
    }

    [[nodiscard]] auto AsyncConnect(int client_fd, const sockaddr* addr, socklen_t addrlen)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const sockaddr* a, const socklen_t al)
        { io_uring_prep_connect(sqe, fd, a, al); };
        return MakeUringAwaitable(*this, prep, &Worker::StatIncConnect, client_fd, addr, addrlen);
    }

    [[nodiscard]] auto AsyncClose(int fd)
    {
        auto prep = [](io_uring_sqe* sqe, int file_fd) { io_uring_prep_close(sqe, file_fd); };
        return MakeUringAwaitable(*this, prep, fd);
    }

    [[nodiscard]] auto AsyncFsync(int fd)
    {
        auto prep = [](io_uring_sqe* sqe, const int file_fd) { io_uring_prep_fsync(sqe, file_fd, 0); };
        return MakeUringAwaitable(*this, prep, fd);
    }

    [[nodiscard]] auto AsyncFdatasync(int fd)
    {
        auto prep = [](io_uring_sqe* sqe, const int file_fd)
        { io_uring_prep_fsync(sqe, file_fd, IORING_FSYNC_DATASYNC); };
        return MakeUringAwaitable(*this, prep, fd);
    }

    [[nodiscard]] auto AsyncFallocate(int fd, int mode, off_t size)
    {
        auto prep = [](io_uring_sqe* sqe, const int file_fd, const int p_mode, const off_t offset, const off_t len)
        { io_uring_prep_fallocate(sqe, file_fd, p_mode, offset, len); };
        return MakeUringAwaitable(*this, prep, fd, mode, static_cast<off_t>(0), size);
    }

    [[nodiscard]] auto AsyncFtruncate(int fd, off_t length)
    {
        auto prep = [](io_uring_sqe* sqe, int file_fd, off_t off) { io_uring_prep_ftruncate(sqe, file_fd, off); };
        return MakeUringAwaitable(*this, prep, fd, length);
    }

    [[nodiscard]] auto AsyncPoll(int fd, int events)
    {
        auto prep = [](io_uring_sqe* sqe, int p_fd, int p_events)
        { io_uring_prep_poll_add(sqe, p_fd, static_cast<unsigned>(p_events)); };
        return MakeUringAwaitable(*this, prep, fd, events);
    }

    [[nodiscard]] auto AsyncSendmsg(int fd, const msghdr* msg, int flags)
    {
        auto prep = [](io_uring_sqe* sqe, int f, const msghdr* m, int fl) { io_uring_prep_sendmsg(sqe, f, m, fl); };
        return MakeUringAwaitable(*this, prep, &Worker::StatIncWrite, fd, msg, flags);
    }

    [[nodiscard]] auto AsyncOpenat(std::filesystem::path path, int flags, mode_t mode)
    {
        auto prep = [](io_uring_sqe* sqe, int dfd, const std::filesystem::path& p, int f, mode_t m)
        { io_uring_prep_openat(sqe, dfd, p.c_str(), f, m); };
        return MakeUringAwaitable(*this, prep, &Worker::StatIncOpen, AT_FDCWD, std::move(path), flags, mode);
    }

    [[nodiscard]] auto AsyncUnlinkAt(int dirfd, std::filesystem::path path, int flags)
    {
        auto prep = [](io_uring_sqe* sqe, int dfd, const std::filesystem::path& p, int f)
        { io_uring_prep_unlinkat(sqe, dfd, p.c_str(), f); };
        return MakeUringAwaitable(*this, prep, dirfd, std::move(path), flags);
    }

    Task<Result<void>> AsyncReadExact(int client_fd, std::span<char> buf);
    Task<Result<void>> AsyncReadExactAt(int fd, std::span<char> buf, uint64_t offset);
    Task<Result<void>> AsyncWriteExact(int client_fd, std::span<const char> buf);
    Task<Result<void>> AsyncWriteExactAt(int client_fd, std::span<const char> buf, uint64_t offset);
    Task<std::expected<void, Error>> AsyncSleep(std::chrono::nanoseconds duration);
    Task<Result<void>> AsyncSendfile(int out_fd, int in_fd, off_t offset, size_t count);
};

struct SwitchToWorker
{
    Worker& worker;

    explicit SwitchToWorker(Worker& worker) : worker(worker) {}

    bool await_ready() const noexcept { return worker.IsOnWorkerThread(); }                           // NOLINT
    void await_suspend(std::coroutine_handle<> h) const { internal::WorkerAccess::Post(worker, h); }  // NOLINT
    void await_resume() const noexcept {}                                                             // NOLINT
};

namespace internal
{
inline void WorkerAccess::Post(Worker& worker, std::coroutine_handle<> h)
{
    worker.Post(h);
}

inline io_uring& WorkerAccess::GetRing(Worker& worker) noexcept
{
    return worker.GetRing();
}
}  // namespace internal

template <typename Prep, typename... Args>
bool IoUringAwaitable<Prep, Args...>::await_suspend(std::coroutine_handle<> h)
{
    assert(worker.IsOnWorkerThread() && "kio::async_* operation was called from the wrong thread.");

    // Acquire state from pool. RefCount starts at 2 (User + Kernel).
    completion_state = CompletionPool::Acquire(h);

    auto& ring = internal::WorkerAccess::GetRing(worker);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);

    if (sqe == nullptr)
    {
        // Failed to get SQE.
        completion_state->result = -EAGAIN;
        // Drop Kernel ref immediately since we aren't giving it to kernel
        completion_state->Release();
        return false;
    }

    std::apply([this, sqe]<typename... T>(T&&... unpacked_args)
               { io_uring_prep(sqe, std::forward<T>(unpacked_args)...); }, std::move(io_args));

    // Pass the heap pointer to the kernel
    io_uring_sqe_set_data(sqe, completion_state);
    return true;
}

}  // namespace kio::io

#endif  // KIO_WORKER_H