//
// Created by Yao ACHI on 17/10/2025.
//
#include "worker.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <liburing.h>
#include <sys/utsname.h>
#include <utility>

#include "async_logger.h"

namespace kio::io
{
void Worker::CheckKernelVersion()
{
    utsname buf{};
    if (uname(&buf) != 0)
    {
        throw std::runtime_error("Failed to get kernel version");
    }

    std::string release(buf.release);
    const size_t dot_pos = release.find('.');
    if (dot_pos == std::string::npos)
    {
        throw std::runtime_error("Failed to parse kernel version");
    }

    if (const int major = std::stoi(release.substr(0, dot_pos)); major < 6)
    {
        throw std::runtime_error("Kernel version must be 6.0.0 or higher");
    }
}

void Worker::CheckSyscallReturn(const int ret)
{
    if (-ret == ETIME || -ret == ETIMEDOUT || -ret == EINTR || -ret == EAGAIN || -ret == EBUSY)
    {
        ALOG_DEBUG("Transient error: {}", strerror(-ret));
        return;
    }

    // real io error
    stats_.io_errors_total++;

    ALOG_ERROR("Fatal I/O error: {}", strerror(-ret));
    throw std::system_error(-ret, std::system_category());
}

int Worker::SubmitSqesWait()
{
    // Define the heartbeat interval (max sleep time)
    __kernel_timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = static_cast<uint32_t>(config_.heartbeat_interval_us * 1000);

    // Busy wait in kernel for this many microseconds before sleeping.
    // This helps maintain high throughput during bursts by avoiding context switches.
    // Requires Kernel 5.12+ (which we satisfy as we check for 6.0+).
    const unsigned min_wait = config_.busy_wait_us;

    io_uring_cqe *cqe_ptr = nullptr;
    // sigmask is null

    // io_uring_submit_and_wait_min_timeout is the hybrid poll primitive.
    // It submits SQEs, then busy loops for 'min_wait' usec looking for completions.
    // If nothing arrives, it sleeps until 'ts' expires or an event arrives.
    const int ret = io_uring_submit_and_wait_min_timeout(&ring_, &cqe_ptr, 1, &ts, min_wait, nullptr);

    if (ret == -ETIME)
    {
        // Heartbeat tick (timeout expired).
        return 0;
    }

    if (ret < 0)
    {
        CheckSyscallReturn(ret);
    }
    return ret;
}

Worker::Worker(const size_t id, const WorkerConfig &config, std::function<void(Worker &)> worker_init_callback) :
    config_(config), id_(id), task_queue_(config.task_queue_capacity),
    worker_init_callback_(std::move(worker_init_callback))
{
    config_.Check();
}

void Worker::Initialize()
{
    io_uring_params params{};
    params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    if (const int ret = io_uring_queue_init_params(static_cast<int>(config_.uring_queue_depth), &ring_, &params);
        ret < 0)
    {
        throw std::system_error(-ret, std::system_category(), "io_uring_queue_init failed");
    }

    ALOG_INFO("IO uring initialized with queue size of {}", config_.uring_queue_depth);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    const uint cpu = id_ % std::thread::hardware_concurrency();
    CPU_SET(cpu, &cpuset);

    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
    {
        ALOG_WARN("Worker {}: Failed to set CPU affinity: {}", id_, strerror(rc));
    }
    else
    {
        ALOG_INFO("Worker {} pinned to CPU {}", id_, cpu);
    }

    if (const int ret = io_uring_register_iowq_aff(&ring_, 1, &cpuset); ret < 0)
    {
        ALOG_WARN("Worker {}: Failed to set io-wq affinity: {}", id_, strerror(-ret));
    }
    else
    {
        ALOG_INFO("Worker {}: io-wq threads pinned to CPU {}", id_, cpu);
    }
}

void Worker::Loop()
{
    // Event Loop Strategy:
    // 1. Drain the Task Queue (fast, lock-free, from other threads)
    // 2. Submit pending IO and Wait (Heartbeat)
    // 3. Process Completions

    while (!stop_token_.stop_requested())
    {
        // 1. Drain Task Queue (with budget)
        // This ensures latency is minimized for tasks posted from other threads.
        std::coroutine_handle<> h;
        size_t resumed = 0;
        // Use config-based budget to alternate between tasks and IO
        while (resumed < config_.task_batch_size && task_queue_.TryPop(h))
        {
            h.resume();
            resumed++;
        }

        // 2. Submit & Wait (Heartbeat)
        // If the task queue processing generated SQEs, this submits them.
        // If nothing happened, we wait for IO or the timeout (Heartbeat).
        // We ignore the return value here as we process completions below regardless.
        SubmitSqesWait();

        // 3. Process Completions
        // We don't need to check the return of submit_sqes_wait because we just
        // want to drain whatever is in the CQ ring.
        ProcessCompletions();
    }

    ALOG_INFO("Worker {} loop shut down completed", this->id_);

    // signal shutdown for anyone waiting
    SignalShutdownComplete();
}

// The main per-thread body that used to be inside the thread lambda.
void Worker::LoopForever()
{
    // the calling thread sets the id
    thread_id_.store(std::this_thread::get_id());
    stop_token_ = this->GetStopToken();

    try
    {
        CheckKernelVersion();
        Initialize();
        SignalInitComplete();

        // Init callback to run inside the worker, respect io_uring single issuer
        if (worker_init_callback_)
        {
            worker_init_callback_(*this);
        }

        ALOG_INFO("Worker {} starting event loop", id_);
        Loop();
        ALOG_INFO("Worker {} exited event loop", id_);
        DrainCompletions();
    }
    catch (const std::exception &e)
    {
        ALOG_ERROR("Worker {} failed during run(): {}", id_, e.what());
        // ensure latches are released so the pool doesn't wait forever
        SignalInitComplete();
        SignalShutdownComplete();
    }
}

unsigned Worker::ProcessCompletions()
{
    io_uring_cqe *cqe = nullptr;
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
    // stop is idempotent
    if (stopped_.exchange(true))
    {
        ALOG_DEBUG("Worker requested to stop but it is already stopped");
        return true;
    }

    assert(!IsOnWorkerThread() && "FATAL: Worker::request_stop called from worker thread - will deadlock!");

    if (IsOnWorkerThread())
    {
        ALOG_ERROR("Worker stop called from worker {} thread", id_);
        ALOG_ERROR("This will cause deadlock. Workers cannot stop themselves.");
        ALOG_ERROR("Shutdown must be initiated from main thread or destructor.");
        ALOG_ERROR("Shutdown signal ignored.");
        return false;
    }

    const auto stop = stop_source_.request_stop();
    // No need to write to eventfd anymore.
    // The worker will wake up at the next heartbeat (max 100us latency)
    // and see the stop_token.
    return stop;
}

Worker::~Worker()
{
    ALOG_DEBUG("Worker {} destructor called", id_);
    // request stop if not already requested

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
        // TODO: have a different strategy, like growing the queue or blocking the producer.
        return;
    }
    // No syscall. We just push.
    // The worker will see this on the next Heartbeat or IO completion.
}

// worker.cpp
void Worker::HandleCqe(io_uring_cqe *cqe)
{
    auto *completion = static_cast<IoCompletion *>(io_uring_cqe_get_data(cqe));
    if (completion == nullptr)
    {
        return;
    }

    completion->Complete(cqe->res);
}

bool Worker::IsOnWorkerThread() const
{
    return std::this_thread::get_id() == thread_id_;
}

Task<Result<void>> Worker::AsyncReadExact(const int client_fd, std::span<char> buf)
{
    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (total_bytes_read < total_to_read)
    {
        const int bytes_read = KIO_TRY(co_await this->AsyncRead(client_fd, buf.subspan(total_bytes_read)));

        if (bytes_read == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }
        total_bytes_read += static_cast<size_t>(bytes_read);
    }
    co_return {};
}

Task<Result<void>> Worker::AsyncReadExactAt(const int fd, std::span<char> buf, uint64_t offset)
{
    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (total_bytes_read < total_to_read)
    {
        // We always pass an offset, and we always increment it.
        const uint64_t current_offset = offset + total_bytes_read;
        std::span<char> remaining_buf = buf.subspan(total_bytes_read);

        const int bytes_read = KIO_TRY(co_await this->AsyncReadAt(fd, remaining_buf, current_offset));
        if (bytes_read == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }
        total_bytes_read += static_cast<size_t>(bytes_read);
    }

    co_return {};
}

Task<Result<void>> Worker::AsyncWriteExact(const int client_fd, std::span<const char> buf)
{
    size_t total_bytes_written = 0;
    const size_t total_to_write = buf.size();

    while (total_bytes_written < total_to_write)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
        const int bytes_written = KIO_TRY(co_await this->AsyncWrite(client_fd, remaining_buf));
        if (bytes_written == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }

        total_bytes_written += static_cast<size_t>(bytes_written);
    }

    co_return {};
}

Task<Result<void>> Worker::AsyncWriteExactAt(const int client_fd, std::span<const char> buf, uint64_t offset)
{
    size_t total_bytes_written = 0;
    const size_t total_to_write = buf.size();

    while (total_bytes_written < total_to_write)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
        const uint64_t current_offset = offset + total_bytes_written;
        const int bytes_written = KIO_TRY(co_await this->AsyncWriteAt(client_fd, remaining_buf, current_offset));
        if (bytes_written == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }

        total_bytes_written += static_cast<size_t>(bytes_written);
    }

    co_return {};
}

Task<std::expected<void, Error>> Worker::AsyncSleep(std::chrono::nanoseconds duration)
{
    __kernel_timespec ts{};
    ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    ts.tv_nsec = (duration % std::chrono::seconds(1)).count();

    auto prep = [](io_uring_sqe *sqe, __kernel_timespec *t, unsigned flags)
    { io_uring_prep_timeout(sqe, t, 0, flags); };

    // We capture the awaitable to inspect the raw result, as await_resume() would
    // treat -ETIME as an error, but here it indicates successful timeout expiry.
    auto awaitable = MakeUringAwaitable(*this, prep, &ts, 0);
    (void) co_await awaitable;

    if (const int res = awaitable.completion.result; res < 0 && res != -ETIME)
    {
        co_return std::unexpected(Error::FromErrno(-res));
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

    const net::FDGuard pipe_read(raw_pipe[0]);
    const net::FDGuard pipe_write(raw_pipe[1]);

    ::fcntl(pipe_read.get(), F_SETFL, O_NONBLOCK);
    ::fcntl(pipe_write.get(), F_SETFL, O_NONBLOCK);

    size_t remaining = count;
    off_t current_offset = offset;

    auto prep_splice = [](io_uring_sqe *sqe, int in, off_t off_in, int out, off_t off_out, unsigned int len)
    { io_uring_prep_splice(sqe, in, off_in, out, off_out, len, 0); };

    while (remaining > 0)
    {
        constexpr size_t k_chunk_size = 65536;
        const size_t to_splice = std::min(remaining, k_chunk_size);

        // Step A: Splice File -> Pipe
        const int res_in =
                KIO_TRY(co_await MakeUringAwaitable(*this, prep_splice, in_fd, current_offset, pipe_write.get(),
                                                    static_cast<off_t>(-1), static_cast<unsigned int>(to_splice)));

        if (res_in == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }

        // Step B: Splice Pipe -> Socket
        auto pipe_remaining = static_cast<size_t>(res_in);
        while (pipe_remaining > 0)
        {
            const int res_out = KIO_TRY(
                    co_await MakeUringAwaitable(*this, prep_splice, pipe_read.get(), static_cast<off_t>(-1), out_fd,
                                                static_cast<off_t>(-1), static_cast<unsigned int>(pipe_remaining)));

            if (res_out == 0)
            {
                co_return std::unexpected(Error{ErrorCategory::kNetwork, EPIPE});
                pipe_remaining -= res_out;
            }

            current_offset += res_in;
            remaining -= res_in;
            stats_.bytes_written_total += res_in;
        }

        stats_.write_ops_total++;
        co_return {};
    }
    co_return {};

}  // namespace kio::io
}  // namespace kio::io
