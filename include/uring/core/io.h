#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include <liburing.h>

#include "awaiter.hpp"
#include "buffer_pool.hpp"
#include "detail/queue.hpp"
#include "task.hpp"
#include "uring/fd.hpp"
#include "uring/net.hpp"

namespace URing
{
struct IoOptions
{
    uint32_t entries = 16800;
    unsigned flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    std::vector<int> worker_cpu_affinity{};
    size_t batch_max_size = 128;
    uint32_t sq_thread_idle_ms = 2000;
    int sq_thread_cpu = -1;
};

// One reactor owns its ring, scheduled roots, pending operations, and buffers.
// schedule() accepts independent roots from any thread. A root and all its
// awaited children execute only on the reactor's owner thread.
class IO
{
    template <typename Setup, typename Mapper>
        requires std::invocable<Setup, io_uring_sqe*> && std::invocable<Mapper, int32_t>
    friend class IoAwaiter;
    template <typename T>
    friend Result<T> sync_wait(IO&, Task<T>&&);

public:
    static constexpr int kDefaultAcceptFlags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    explicit IO(size_t id, const IO* leader = nullptr, const IoOptions& opts = {},
                std::initializer_list<BucketConfig> buffers = {});
    IO(const IO&) = delete;
    IO& operator=(const IO&) = delete;
    IO(IO&&) = delete;
    IO& operator=(IO&&) = delete;
    ~IO();

    // Runs once until stop, then rejects submissions, cancels pending I/O, and
    // resumes accepted task chains until they unwind. Does not restart.
    void run(std::stop_token stop);
    // Returns false after shutdown begins; the unstarted task is then destroyed.
    bool schedule(Task<void> task);
    [[nodiscard]] size_t id() const noexcept { return id_; }
    // Borrowed buffers must be used and returned on this worker. IO outlives them.
    [[nodiscard]] Result<FixedBuffer> take_fixed_buffer(size_t size);

    [[nodiscard]] auto accept(Fd& fd, int flags = kDefaultAcceptFlags)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), flags](io_uring_sqe* sqe)
            { io_uring_prep_accept(sqe, raw, nullptr, nullptr, flags); }, ResumeFd{});
    }
    [[nodiscard]] auto accept(Fd& fd, SocketAddress& addr, int flags = kDefaultAcceptFlags)
    {
        return IoAwaiter(
            *this,
            [raw = fd.Get(), &addr, flags](io_uring_sqe* sqe)
            {
                addr.addrlen = sizeof(sockaddr_storage);
                io_uring_prep_accept(sqe, raw, addr.GetMutable(), &addr.addrlen, flags);
            },
            ResumeFd{});
    }
    [[nodiscard]] auto connect(Fd& fd, SocketAddress addr)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), addr](io_uring_sqe* sqe)
            { io_uring_prep_connect(sqe, raw, addr.Get(), addr.addrlen); }, detail::ResumeVoid{});
    }
    [[nodiscard]] auto read(Fd& fd, std::span<std::byte> buf, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), buf, offset](io_uring_sqe* sqe)
            { io_uring_prep_read(sqe, raw, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
    }
    [[nodiscard]] auto write(Fd& fd, std::span<const std::byte> buf, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), buf, offset](io_uring_sqe* sqe)
            { io_uring_prep_write(sqe, raw, buf.data(), buf.size(), offset); }, detail::ResumeInt{});
    }
    [[nodiscard]] auto readv(Fd& fd, std::span<const iovec> vecs, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), vecs, offset](io_uring_sqe* sqe)
            { io_uring_prep_readv(sqe, raw, vecs.data(), static_cast<unsigned>(vecs.size()), offset); },
            detail::ResumeInt{});
    }
    [[nodiscard]] auto writev(Fd& fd, std::span<const iovec> vecs, off_t offset = -1)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), vecs, offset](io_uring_sqe* sqe)
            { io_uring_prep_writev(sqe, raw, vecs.data(), static_cast<unsigned>(vecs.size()), offset); },
            detail::ResumeInt{});
    }
    [[nodiscard]] auto open(std::filesystem::path path, int flags, mode_t mode = 0644)
    {
        return IoAwaiter(
            *this, [path = std::move(path), flags, mode](io_uring_sqe* sqe)
            { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); }, ResumeFd{});
    }
    [[nodiscard]] auto close(Fd&& fd)
    {
        // Once submitted, the kernel owns the descriptor. Drain rather than
        // cancel close so shutdown cannot abandon that ownership transfer.
        return IoAwaiter(
            *this, [fd = std::move(fd)](io_uring_sqe* sqe) mutable { io_uring_prep_close(sqe, fd.Release()); },
            detail::ResumeVoid{}, 0, false);
    }
    [[nodiscard]] auto remove(std::filesystem::path path)
    {
        return IoAwaiter(
            *this, [path = std::move(path)](io_uring_sqe* sqe)
            { io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0); }, detail::ResumeVoid{});
    }
    [[nodiscard]] auto rename(std::filesystem::path from, std::filesystem::path to)
    {
        return IoAwaiter(
            *this, [from = std::move(from), to = std::move(to)](io_uring_sqe* sqe)
            { io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0); }, detail::ResumeVoid{});
    }
    [[nodiscard]] auto fsync(Fd& fd, bool full_sync = false)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), full_sync](io_uring_sqe* sqe)
            { io_uring_prep_fsync(sqe, raw, full_sync ? 0u : IORING_FSYNC_DATASYNC); }, detail::ResumeVoid{});
    }
    [[nodiscard]] auto fallocate(Fd& fd, int mode, off_t offset, off_t len)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), mode, offset, len](io_uring_sqe* sqe)
            { io_uring_prep_fallocate(sqe, raw, mode, offset, len); }, detail::ResumeVoid{});
    }
    [[nodiscard]] auto ftruncate(Fd& fd, off_t len)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), len](io_uring_sqe* sqe) { io_uring_prep_ftruncate(sqe, raw, len); },
            detail::ResumeVoid{});
    }
    [[nodiscard]] auto poll(Fd& fd, unsigned mask)
    {
        return IoAwaiter(
            *this, [raw = fd.Get(), mask](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, raw, mask); },
            detail::ResumeInt{});
    }
    template <typename Rep, typename Period>
    [[nodiscard]] auto sleep(std::chrono::duration<Rep, Period> duration)
    {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        return IoAwaiter(
            *this, [ts = __kernel_timespec{ns / 1'000'000'000, ns % 1'000'000'000}](io_uring_sqe* sqe)
            { io_uring_prep_timeout(sqe, &ts, 0, 0); },
            [](int32_t res) -> Result<void>
            {
                if (res == -ETIME || res == 0)
                    return {};
                return error_from_errno(res);
            });
    }
    [[nodiscard]] auto read_fixed(Fd& fd, FixedBuffer& buf, const size_t len, off_t offset)
    {
        const int error = buf.pool_ == &buffer_pool_ ? 0 : -EINVAL;
        return IoAwaiter(
            *this,
            [raw = fd.Get(), ptr = buf.ptr(), len = std::min(len, buf.size()), index = buf.index_,
             offset](io_uring_sqe* sqe) { io_uring_prep_read_fixed(sqe, raw, ptr, len, offset, index); },
            detail::ResumeInt{}, error);
    }
    [[nodiscard]] auto read_fixed(Fd& fd, FixedBuffer& buf, off_t offset = -1)
    {
        return read_fixed(fd, buf, buf.size(), offset);
    }
    [[nodiscard]] auto write_fixed(Fd& fd, const FixedBuffer& buf, size_t len, off_t offset)
    {
        const int error = buf.pool_ == &buffer_pool_ ? 0 : -EINVAL;
        return IoAwaiter(
            *this,
            [raw = fd.Get(), ptr = buf.ptr(), len = std::min(len, buf.size()), index = buf.index_,
             offset](io_uring_sqe* sqe) { io_uring_prep_write_fixed(sqe, raw, ptr, len, offset, index); },
            detail::ResumeInt{}, error);
    }
    [[nodiscard]] auto write_fixed(Fd& fd, const FixedBuffer& buf, off_t offset = -1)
    {
        return write_fixed(fd, buf, buf.size(), offset);
    }

private:
    struct ResumeFd
    {
        Result<Fd> operator()(const int32_t res) const noexcept
        {
            if (res < 0)
                return error_from_errno(res);
            return Fd{res};
        }
    };
    static constexpr uint64_t kWakeTag = 1;
    io_uring ring_{};
    int wake_fd_ = -1;
    uint64_t wake_value_ = 0;
    IoOptions opts_;
    size_t id_;
    FixedBufferPool buffer_pool_;
    std::thread::id owner_thread_{};
    bool activated_ = false;
    bool running_ = false;
    bool stopping_ = false;
    bool wake_armed_ = false;
    // Covers admission + publication: shutdown cannot race a producer still
    // linking an accepted root into the intrusive MPSC queue.
    std::mutex admission_mutex_;
    bool accepting_ = true;
    detail::CoroQueue incoming_;
    std::deque<std::coroutine_handle<>> ready_;
    detail::TaskPromiseBase* roots_ = nullptr;
    detail::TaskPromiseBase* completed_ = nullptr;
    IoOps* pending_ = nullptr;

    void activate();
    void tick();
    void shutdown();
    void adopt_roots();
    void cancel_pending();
    void reap_completed() noexcept;
    void assert_owner() const;
    void wake() const noexcept;
    void arm_wake_read();
    io_uring_sqe* get_sqe() noexcept;
    void track(IoOps& op) noexcept;
    void untrack(IoOps& op) noexcept;
    static void complete_root(detail::TaskPromiseBase& promise) noexcept;
};

template <typename Setup, typename Mapper>
    requires std::invocable<Setup, io_uring_sqe*> && std::invocable<Mapper, int32_t>
template <typename Promise>
std::coroutine_handle<> IoAwaiter<Setup, Mapper>::await_suspend(std::coroutine_handle<Promise> h) noexcept
{
    if (h.promise().owner != &io_)
    {
        ops_.res = -EXDEV;
        return h;
    }
    io_.assert_owner();
    if (io_.stopping_ || error_ != 0)
    {
        ops_.res = io_.stopping_ ? -ECANCELED : error_;
        return h;
    }
    auto* sqe = io_.get_sqe();
    if (sqe == nullptr)
    {
        ops_.res = -EAGAIN;
        return h;
    }
    ops_.h = h;
    setup_(sqe);
    io_.track(ops_);
    io_uring_sqe_set_data(sqe, &ops_);
    return std::noop_coroutine();
}

namespace detail
{
// References are coroutine parameters, so no temporary capturing lambda can
// leave a suspended coroutine holding a pointer to a destroyed closure.
template <typename T>
Task<void> wait_for(Task<T> task, std::optional<Result<T>>& result, bool& done)
{
    result.emplace(co_await task);
    done = true;
    co_return {};
}
}  // namespace detail

// Testing helper. Repeated calls use the same owner thread. Public run()
// shutdown is terminal; sync_wait cannot revive a stopped reactor.
template <typename T>
Result<T> sync_wait(IO& io, Task<T>&& task)
{
    if (io.running_)
        throw std::logic_error("sync_wait cannot nest inside a running reactor");
    if (io.stopping_)
        return error_from_errno(ECANCELED);
    io.activate();
    bool done = false;
    std::optional<Result<T>> result;
    if (!io.schedule(detail::wait_for(std::move(task), result, done)))
        return error_from_errno(ECANCELED);
    io.running_ = true;
    try
    {
        while (!done)
            io.tick();
    }
    catch (...)
    {
        io.shutdown();
        io.running_ = false;
        throw;
    }
    io.running_ = false;
    return std::move(*result);
}
}  // namespace URing
