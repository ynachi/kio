//
// Created by Yao ACHI on 17/10/2025.
//
#include "worker.h"

#include "async_logger.h"

#include <chrono>
#include <functional>
#include <utility>

#include <fcntl.h>
#include <liburing.h>

#include <sys/utsname.h>

namespace kio::io
{
void Worker::CheckKernelVersion()
{
    utsname buf{};
    if (uname(&buf) != 0)
    {
        throw std::runtime_error("Failed to get kernel version");
    }

    std::string const kRelease(buf.release);
    const size_t kDotPos = kRelease.find('.');
    if (kDotPos == std::string::npos)
    {
        throw std::runtime_error("Failed to parse kernel version");
    }

    if (const int kMajor = std::stoi(kRelease.substr(0, kDotPos)); kMajor < 6)
    {
        throw std::runtime_error("Kernel version must be 6.0.0 or higher");
    }
}

void Worker::CheckSyscallReturn(const int ret)
{
    // These are not fatal errors for the *Loop wait*.
    if (-ret == ETIME || -ret == ETIMEDOUT || -ret == EINTR || -ret == EAGAIN || -ret == EBUSY)
    {
        ALOG_DEBUG("Transient error in wait: {}", strerror(-ret));
        return;
    }

    stats_.io_errors_total++;
    ALOG_ERROR("Fatal I/O error in wait: {}", strerror(-ret));
    throw std::system_error(-ret, std::system_category());
}

int Worker::SubmitSqesWait(const uint32_t wait_us)
{
    // Define the heartbeat interval (max sleep time)
    __kernel_timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = wait_us * 1000;

    // DYNAMIC BUSY WAIT:
    // Only engage the kernel-side busy loop if we are actually submitting new work.
    // If we are just checking for completions (idle/heartbeat), sleep immediately.
    unsigned min_wait = config_.busy_wait_us;

    if (io_uring_sq_ready(&ring_) == 0)
    {
        min_wait = 0;
    }

    io_uring_cqe* cqe_ptr = nullptr;

    return io_uring_submit_and_wait_min_timeout(&ring_, &cqe_ptr, 1, &ts, min_wait, nullptr);
}

Worker::Worker(const size_t id, const WorkerConfig& config, std::function<void(Worker&)> worker_init_callback)
    : config_(config),
      id_(id),
      task_queue_(config.task_queue_capacity),
      worker_init_callback_(std::move(worker_init_callback))
{
    config_.Check();
}

void Worker::Initialize()
{
    io_uring_params params{};
    params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    if (const int kRet = io_uring_queue_init_params(static_cast<int>(config_.uring_queue_depth), &ring_, &params);
        kRet < 0)
    {
        throw std::system_error(-kRet, std::system_category(), "io_uring_queue_init failed");
    }

    ALOG_INFO("IO uring initialized with queue size of {}", config_.uring_queue_depth);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    const uint kCpu = id_ % std::thread::hardware_concurrency();
    CPU_SET(kCpu, &cpuset);

    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
    {
        ALOG_WARN("Worker {}: Failed to set CPU affinity: {}", id_, strerror(rc));
    }
    else
    {
        ALOG_INFO("Worker {} pinned to CPU {}", id_, kCpu);
    }

    if (const int ret = io_uring_register_iowq_aff(&ring_, 1, &cpuset); ret < 0)
    {
        ALOG_WARN("Worker {}: Failed to set io-wq affinity: {}", id_, strerror(-ret));
    }
    else
    {
        ALOG_INFO("Worker {}: io-wq threads pinned to CPU {}", id_, kCpu);
    }
}

void Worker::Loop()
{
    uint32_t current_wait_us = config_.heartbeat_interval_us;

    while (!stop_token_.stop_requested())
    {
        std::coroutine_handle<> h;
        size_t resumed = 0;
        while (resumed < config_.task_batch_size && task_queue_.TryPop(h))
        {
            h.resume();
            resumed++;
        }

        SubmitSqesWait(current_wait_us);
        unsigned completions = ProcessCompletions();

        // Adaptive Heartbeat:
        // If we did work (resumed tasks OR processed IO), go fast (100us).
        // If we did nothing, exponentially back off to save CPU (up to 2ms).
        if (resumed > 0 || completions > 0)
        {
            current_wait_us = config_.heartbeat_interval_us;
        }
        else
        {
            current_wait_us = std::min(current_wait_us * 2, config_.max_idle_wait_us);
        }
    }

    ALOG_INFO("Worker {} loop shut down completed", this->id_);
    SignalShutdownComplete();
}

void Worker::LoopForever()
{
    thread_id_.store(std::this_thread::get_id());
    stop_token_ = this->GetStopToken();

    try
    {
        CheckKernelVersion();
        Initialize();
        SignalInitComplete();

        if (worker_init_callback_)
        {
            worker_init_callback_(*this);
        }

        ALOG_INFO("Worker {} starting event loop", id_);
        Loop();
        ALOG_INFO("Worker {} exited event loop", id_);
        DrainCompletions();
    }
    catch (const std::exception& e)
    {
        ALOG_ERROR("Worker {} failed during run(): {}", id_, e.what());
        SignalInitComplete();
        SignalShutdownComplete();
    }
}

unsigned Worker::ProcessCompletions()
{
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        count++;
        HandleCqe(cqe);
    }

    io_uring_cq_advance(&ring_, count);
    return count;
}

void Worker::DrainCompletions()
{
    ProcessCompletions();
}

bool Worker::RequestStop()
{
    if (stopped_.exchange(true))
    {
        ALOG_DEBUG("Worker requested to stop but it is already stopped");
        return true;
    }

    assert(!IsOnWorkerThread() && "FATAL: Worker::request_stop called from worker thread - will deadlock!");

    if (IsOnWorkerThread())
    {
        ALOG_ERROR("Worker stop called from worker {} thread", id_);
        return false;
    }

    const auto kStop = stop_source_.request_stop();
    return kStop;
}

Worker::~Worker()
{
    ALOG_DEBUG("Worker {} destructor called", id_);

    if (this->ring_.ring_fd >= 0)
    {
        io_uring_queue_exit(&ring_);
    }

    ALOG_DEBUG("Worker {} destructor finished", id_);
}

void Worker::Post(std::coroutine_handle<> h)
{
    if (!task_queue_.TryPush(h))
    {
        ALOG_ERROR("Worker {} task queue is full. Dropping task.", id_);
        return;
    }
}

void Worker::HandleCqe(io_uring_cqe* cqe)
{
    // The data pointer is now a SafeIoCompletion*
    auto* completion = static_cast<SafeIoCompletion*>(io_uring_cqe_get_data(cqe));
    if (completion == nullptr)
    {
        return;
    }

    // Complete() will check if the user has abandoned the task and handle cleanup
    completion->Complete(cqe->res);
}

bool Worker::IsOnWorkerThread() const
{
    return std::this_thread::get_id() == thread_id_;
}

Task<Result<void>> Worker::AsyncReadExact(const int client_fd, std::span<char> buf)
{
    size_t total_bytes_read = 0;
    const size_t kTotalToRead = buf.size();

    while (total_bytes_read < kTotalToRead)
    {
        const int kBytesRead = KIO_TRY(co_await this->AsyncRead(client_fd, buf.subspan(total_bytes_read)));

        if (kBytesRead == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }
        total_bytes_read += static_cast<size_t>(kBytesRead);
    }
    co_return {};
}

Task<Result<void>> Worker::AsyncReadExactAt(const int fd, std::span<char> buf, uint64_t offset)
{
    size_t total_bytes_read = 0;
    const size_t kTotalToRead = buf.size();

    while (total_bytes_read < kTotalToRead)
    {
        const uint64_t kCurrentOffset = offset + total_bytes_read;
        std::span<char> remaining_buf = buf.subspan(total_bytes_read);

        const int kBytesRead = KIO_TRY(co_await this->AsyncReadAt(fd, remaining_buf, kCurrentOffset));
        if (kBytesRead == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }
        total_bytes_read += static_cast<size_t>(kBytesRead);
    }

    co_return {};
}

Task<Result<void>> Worker::AsyncWriteExact(const int client_fd, std::span<const char> buf)
{
    size_t total_bytes_written = 0;
    const size_t kTotalToWrite = buf.size();

    while (total_bytes_written < kTotalToWrite)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
        const int kBytesWritten = KIO_TRY(co_await this->AsyncWrite(client_fd, remaining_buf));
        if (kBytesWritten == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }

        total_bytes_written += static_cast<size_t>(kBytesWritten);
    }

    co_return {};
}

Task<Result<void>> Worker::AsyncWriteExactAt(const int client_fd, std::span<const char> buf, uint64_t offset)
{
    size_t total_bytes_written = 0;
    const size_t kTotalToWrite = buf.size();

    while (total_bytes_written < kTotalToWrite)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
        const uint64_t kCurrentOffset = offset + total_bytes_written;
        const int kBytesWritten = KIO_TRY(co_await this->AsyncWriteAt(client_fd, remaining_buf, kCurrentOffset));
        if (kBytesWritten == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }

        total_bytes_written += static_cast<size_t>(kBytesWritten);
    }

    co_return {};
}

Task<std::expected<void, Error>> Worker::AsyncSleep(std::chrono::nanoseconds duration)
{
    __kernel_timespec ts{};
    ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    ts.tv_nsec = (duration % std::chrono::seconds(1)).count();

    auto prep = [](io_uring_sqe* sqe, __kernel_timespec* t, unsigned flags)
    { io_uring_prep_timeout(sqe, t, 0, flags); };

    auto awaitable = MakeUringAwaitable(*this, prep, &ts, 0);
    auto res = co_await awaitable;

    if (!res)
    {
        if (res.error().value == ETIME)
        {
            co_return {};
        }
        co_return std::unexpected(res.error());
    }

    co_return {};
}

Task<Result<void>> Worker::AsyncSendfile(int out_fd, int in_fd, off_t offset, size_t count)
{
    int raw_pipe[2];
    if (::pipe(raw_pipe) < 0)
    {
        co_return std::unexpected(Error::FromErrno(errno));
    }

    const net::FDGuard kPipeRead(raw_pipe[0]);
    const net::FDGuard kPipeWrite(raw_pipe[1]);

    ::fcntl(kPipeRead.get(), F_SETFL, O_NONBLOCK);
    ::fcntl(kPipeWrite.get(), F_SETFL, O_NONBLOCK);

    size_t remaining = count;
    off_t current_offset = offset;

    auto prep_splice = [](io_uring_sqe* sqe, int in, off_t off_in, int out, off_t off_out, unsigned int len)
    { io_uring_prep_splice(sqe, in, off_in, out, off_out, len, 0); };

    while (remaining > 0)
    {
        constexpr size_t kChunkSize = 65536;
        const size_t kToSplice = std::min(remaining, kChunkSize);

        const int kResIn =
            KIO_TRY(co_await MakeUringAwaitable(*this, prep_splice, in_fd, current_offset, kPipeWrite.get(),
                                                static_cast<off_t>(-1), static_cast<unsigned int>(kToSplice)));

        if (kResIn == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }

        auto pipe_remaining = static_cast<size_t>(kResIn);
        while (pipe_remaining > 0)
        {
            const int res_out =
                KIO_TRY(co_await MakeUringAwaitable(*this, prep_splice, kPipeRead.get(), static_cast<off_t>(-1), out_fd,
                                                    static_cast<off_t>(-1), static_cast<unsigned int>(pipe_remaining)));

            if (res_out == 0)
            {
                co_return std::unexpected(Error{ErrorCategory::kNetwork, EPIPE});
            }

            pipe_remaining -= static_cast<size_t>(res_out);
            stats_.bytes_written_total += res_out;
        }

        current_offset += kResIn;
        remaining -= kResIn;
        stats_.write_ops_total++;
    }
    co_return {};
}

}  // namespace kio::io