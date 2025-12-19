//
// Created by Yao ACHI on 17/10/2025.
//
#include "worker.h"

#include <chrono>
#include <fcntl.h>
#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/utsname.h>
#include <utility>

#include "async_logger.h"
#include "uring_awaitable.h"

namespace kio::io
{
    void Worker::check_kernel_version()
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

    void Worker::check_syscall_return(const int ret)
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


    int Worker::submit_sqes_wait()
    {
        const int ret = io_uring_submit_and_wait(&ring_, 1);
        if (ret < 0) check_syscall_return(ret);
        return ret;
    }


    Worker::Worker(const size_t id, const WorkerConfig &config, std::function<void(Worker &)> worker_init_callback) :
        config_(config), id_(id), task_queue_(config.task_queue_capacity), worker_init_callback_(std::move(worker_init_callback))
    {
        config_.check();

        wakeup_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wakeup_fd_ < 0)
        {
            throw std::system_error(errno, std::system_category(), "failed to create eventfd");
        }
    }

    void Worker::initialize()
    {
        io_uring_params params{};
        params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

        if (const int ret = io_uring_queue_init_params(static_cast<int>(config_.uring_queue_depth), &ring_, &params); ret < 0)
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

        // Submit the first read on the eventfd to listen for wake-ups
        submit_wakeup_read();

        // Submit immediately so it's pending before loop starts
        int ret = io_uring_submit(&ring_);
        if (ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "Failed to submit initial wakeup read");
        }

        ALOG_DEBUG("Worker {}: Wakeup mechanism initialized ({} ops submitted)", id_, ret);
    }

    // TODO: manage submission errors
    void Worker::loop()
    {
        // event loop (split out to reuse the same logic as before)
        while (!stop_token_.stop_requested())
        {
            // Non-blocking submit
            auto ret = io_uring_submit(&ring_);
            if (ret < 0) check_syscall_return(ret);

            // Get events with DEFER_TASKRUN
            ret = io_uring_get_events(&ring_);
            if (ret < 0) check_syscall_return(ret);
            if (const auto count = process_completions(); count == 0)
            {
                submit_sqes_wait();
            }
        }

        ALOG_INFO("Worker {} loop shut down completed", this->id_);

        // signal shutdown for anyone waiting
        signal_shutdown_complete();
    }

    // The main per-thread body that used to be inside the thread lambda.
    void Worker::loop_forever()
    {
        // the calling thread sets the id
        thread_id_.store(std::this_thread::get_id());
        stop_token_ = this->get_stop_token();

        try
        {
            check_kernel_version();
            initialize();
            signal_init_complete();

            // Init callback to run inside the worker, respect io_uring single issuer
            if (worker_init_callback_)
            {
                worker_init_callback_(*this);
            }

            ALOG_INFO("Worker {} starting event loop", id_);
            loop();
            ALOG_INFO("Worker {} exited event loop", id_);
            drain_completions();
        }
        catch (const std::exception &e)
        {
            ALOG_ERROR("Worker {} failed during run(): {}", id_, e.what());
            // ensure latches are released so the pool doesn't wait forever
            signal_init_complete();
            signal_shutdown_complete();
        }
    }

    unsigned Worker::process_completions()
    {
        io_uring_cqe *cqe;
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            handle_cqe(cqe);
        }

        io_uring_cq_advance(&ring_, count);
        return count;
    }

    void Worker::drain_completions() { process_completions(); }

    bool Worker::request_stop()
    {
        // stop is idempotent
        if (stopped_.exchange(true))
        {
            ALOG_DEBUG("Worker requested to stop but it is already stopped");
            return true;
        }

        assert(!is_on_worker_thread() && "FATAL: Worker::request_stop called from worker thread - will deadlock!");

        if (is_on_worker_thread())
        {
            ALOG_ERROR("Worker stop called from worker {} thread", id_);
            ALOG_ERROR("This will cause deadlock. Workers cannot stop themselves.");
            ALOG_ERROR("Shutdown must be initiated from main thread or destructor.");
            ALOG_ERROR("Shutdown signal ignored.");
            return false;
        }

        const auto stop = stop_source_.request_stop();
        // submit a wakeup write to wake up the io uring ring
        wakeup_write();
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

        if (wakeup_fd_ >= 0)
        {
            ::close(wakeup_fd_);
            // prevent double close
            wakeup_fd_ = -1;
        }
        ALOG_DEBUG("Worker {} destructor finished", id_);
    }

    bool Worker::submit_wakeup_read()
    {
        auto *sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
        {
            ALOG_DEBUG("Io Uring queue full, worker={}", id_);
            return false;
        }

        io_uring_prep_read(sqe, wakeup_fd_, &wakeup_buffer_, sizeof(wakeup_buffer_), 0);
        io_uring_sqe_set_data(sqe, &wakeup_completion_);
        return true;
    }

    void Worker::wakeup_write() const
    {
        constexpr uint64_t value = 1;
        const auto ret = ::write(wakeup_fd_, &value, sizeof(value));
        if (ret != sizeof(value))
        {
            ALOG_ERROR("Failed to write to wakeup eventfd: {}", strerror(errno));
        }
    }

    void Worker::post(std::coroutine_handle<> h)
    {
        if (!task_queue_.try_push(h))
        {
            ALOG_ERROR("Worker {} task queue is full. Dropping task.", id_);
            // TODO: have a different strategy, like growing the queue or blocking the producer.
            return;
        }

        // Signal the worker to wake up
        if (!wakeup_pending_.exchange(true, std::memory_order_acq_rel))
        {
            constexpr uint64_t val = 1;
            ::write(wakeup_fd_, &val, sizeof(val));
        }
    }

    // worker.cpp
    void Worker::handle_cqe(io_uring_cqe *cqe)
    {
        auto *completion = static_cast<IoCompletion *>(io_uring_cqe_get_data(cqe));
        if (completion == nullptr) return;

        if (completion != &wakeup_completion_)
        {
            completion->complete(cqe->res);
            return;
        }

        std::coroutine_handle<> h;
        constexpr int kMaxResumes = 64;
        int resumed = 0;

        while (resumed < kMaxResumes && task_queue_.try_pop(h))
        {
            wakeup_pending_.store(false, std::memory_order_release);
            ++resumed;
            h.resume();
        }
        // while (task_queue_.try_pop(h))
        // {
        //     wakeup_pending_.store(false, std::memory_order_release);
        //     h.resume();
        // }

        // TODO: eventualy stop trying
        if (!stop_token_.stop_requested())
        {
            while (!submit_wakeup_read())
            {
                io_uring_submit(&ring_);
            }
        }
    }

    bool Worker::is_on_worker_thread() const { return std::this_thread::get_id() == thread_id_; }

    Task<Result<void>> Worker::async_read_exact(const int client_fd, std::span<char> buf)
    {
        size_t total_bytes_read = 0;
        const size_t total_to_read = buf.size();

        while (total_bytes_read < total_to_read)
        {
            const int bytes_read = KIO_TRY(co_await this->async_read(client_fd, buf.subspan(total_bytes_read)));

            if (bytes_read == 0)
            {
                co_return std::unexpected(Error{ErrorCategory::File, kIoEof});
            }
            total_bytes_read += static_cast<size_t>(bytes_read);
        }
        co_return {};
    }

    Task<Result<void>> Worker::async_read_exact_at(const int fd, std::span<char> buf, uint64_t offset)
    {
        size_t total_bytes_read = 0;
        const size_t total_to_read = buf.size();

        while (total_bytes_read < total_to_read)
        {
            // We always pass an offset, and we always increment it.
            const uint64_t current_offset = offset + total_bytes_read;
            std::span<char> remaining_buf = buf.subspan(total_bytes_read);

            const int bytes_read = KIO_TRY(co_await this->async_read_at(fd, remaining_buf, current_offset));
            if (bytes_read == 0)
            {
                co_return std::unexpected(Error{ErrorCategory::File, kIoEof});
            }
            total_bytes_read += static_cast<size_t>(bytes_read);
        }

        co_return {};
    }

    Task<Result<void>> Worker::async_write_exact(const int client_fd, std::span<const char> buf)
    {
        size_t total_bytes_written = 0;
        const size_t total_to_write = buf.size();

        while (total_bytes_written < total_to_write)
        {
            std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
            const int bytes_written = KIO_TRY(co_await this->async_write(client_fd, remaining_buf));
            if (bytes_written == 0)
            {
                co_return std::unexpected(Error{ErrorCategory::File, kIoEof});
            }

            total_bytes_written += static_cast<size_t>(bytes_written);
        }

        co_return {};
    }

    Task<Result<void>> Worker::async_write_exact_at(const int client_fd, std::span<const char> buf, uint64_t offset)
    {
        size_t total_bytes_written = 0;
        const size_t total_to_write = buf.size();

        while (total_bytes_written < total_to_write)
        {
            std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
            const uint64_t current_offset = offset + total_bytes_written;
            const int bytes_written = KIO_TRY(co_await this->async_write_at(client_fd, remaining_buf, current_offset));
            if (bytes_written == 0)
            {
                co_return std::unexpected(Error{ErrorCategory::File, kIoEof});
            }

            total_bytes_written += static_cast<size_t>(bytes_written);
        }

        co_return {};
    }

    Task<std::expected<void, Error>> Worker::async_sleep(std::chrono::nanoseconds duration)
    {
        __kernel_timespec ts{};
        ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        ts.tv_nsec = (duration % std::chrono::seconds(1)).count();

        auto prep = [](io_uring_sqe *sqe, __kernel_timespec *t, unsigned flags) { io_uring_prep_timeout(sqe, t, 0, flags); };

        // We capture the awaitable to inspect the raw result, as await_resume() would
        // treat -ETIME as an error, but here it indicates successful timeout expiry.
        auto awaitable = make_uring_awaitable(*this, prep, &ts, 0);
        (void) co_await awaitable;

        if (const int res = awaitable.completion_.result; res < 0 && res != -ETIME)
        {
            co_return std::unexpected(Error::from_errno(-res));
        }
        co_return {};
    }

    Task<Result<void>> Worker::async_sendfile(int out_fd, int in_fd, off_t offset, size_t count)
    {
        int raw_pipe[2];
        if (::pipe(raw_pipe) < 0)
        {
            co_return std::unexpected(Error::from_errno(errno));
        }

        const net::FDGuard pipe_read(raw_pipe[0]);
        const net::FDGuard pipe_write(raw_pipe[1]);

        ::fcntl(pipe_read.get(), F_SETFL, O_NONBLOCK);
        ::fcntl(pipe_write.get(), F_SETFL, O_NONBLOCK);

        size_t remaining = count;
        off_t current_offset = offset;

        auto prep_splice = [](io_uring_sqe *sqe, int in, off_t off_in, int out, off_t off_out, unsigned int len) { io_uring_prep_splice(sqe, in, off_in, out, off_out, len, 0); };

        while (remaining > 0)
        {
            constexpr size_t kChunkSize = 65536;
            const size_t to_splice = std::min(remaining, kChunkSize);

            // Step A: Splice File -> Pipe
            const int res_in = KIO_TRY(co_await make_uring_awaitable(*this, prep_splice, in_fd, current_offset, pipe_write.get(), static_cast<off_t>(-1), static_cast<unsigned int>(to_splice)));

            if (res_in == 0) co_return std::unexpected(Error{ErrorCategory::File, kIoEof});

            // Step B: Splice Pipe -> Socket
            auto pipe_remaining = static_cast<size_t>(res_in);
            while (pipe_remaining > 0)
            {
                const int res_out =
                        KIO_TRY(co_await make_uring_awaitable(*this, prep_splice, pipe_read.get(), static_cast<off_t>(-1), out_fd, static_cast<off_t>(-1), static_cast<unsigned int>(pipe_remaining)));

                if (res_out == 0) co_return std::unexpected(Error{ErrorCategory::Network, EPIPE});
                pipe_remaining -= res_out;
            }

            current_offset += res_in;
            remaining -= res_in;
            stats_.bytes_written_total += res_in;
        }

        stats_.write_ops_total++;
        co_return {};
    }

}  // namespace kio::io
