//
// Created by Yao ACHI on 17/10/2025.
//
#include "kio/include/io/worker.h"

#include <chrono>
#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/utsname.h>

#include "kio/include/async_logger.h"
#include "kio/include/io/uring_awaitable.h"

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
        if (ret >= 0) return;

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

    [[nodiscard]]
    uint64_t Worker::get_op_id()
    {
        if (!free_op_ids.empty())
        {
            const auto id = free_op_ids.back();
            free_op_ids.pop_back();
            return id;
        }
        // TODO: manage the growth better
        // TODO: add metric for pool growth here
        // TODO: Log here
        // Grow pool (simple strategy: double capacity)
        if (op_data_pool_.size() >= config_.max_op_slots)
        {
            ALOG_ERROR("Op slot pool exhausted ({} ops). Consider increasing default_op_slots.", op_data_pool_.size());
            throw std::runtime_error("Op slot pool exhausted");
        }

        const auto cur_size = op_data_pool_.size();
        size_t new_size = std::min(static_cast<size_t>(static_cast<float>(cur_size) * config_.op_slots_growth_factor), config_.max_op_slots);
        ALOG_DEBUG("Growing op_data_pool_ from {} to {}", cur_size, new_size);
        op_data_pool_.resize(new_size);
        for (size_t i = cur_size; i < new_size; ++i)
        {
            free_op_ids.push_back(i);
        }

        stats_.coroutines_pool_resize_total++;

        const auto id = free_op_ids.back();
        free_op_ids.pop_back();
        return id;
    }

    void Worker::release_op_id(const uint64_t op_id) noexcept
    {
        if (op_id < op_data_pool_.size())
        {
            op_data_pool_[op_id] = {};
            free_op_ids.push_back(op_id);
        }
        else
        {
            ALOG_WARN("Tried to release invalid op_id={}", op_id);
        }
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

        // pre-allocate
        op_data_pool_.resize(config_.default_op_slots);
        free_op_ids.reserve(config_.default_op_slots);
        for (size_t i = 0; i < config_.default_op_slots; ++i)
        {
            free_op_ids.push_back(i);
        }
    }

    void Worker::initialize()
    {
        io_uring_params params{};
        params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

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
            if (auto sqe_num = submit_sqes_wait(); sqe_num > 0)
            {
                ALOG_TRACE("Worker {} submitted {} operations", id_, sqe_num);
            }
            process_completions();
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
        // save the stop token
        stop_token_ = this->get_stop_token();

        try
        {
            check_kernel_version();

            initialize();

            // This guarantees that any thread waiting on `wait_ready()` will see
            // the fully initialized state, including the correct thread_id.
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
            this->wait_ready();
            this->wait_shutdown();
        }
    }

    int Worker::submit_sqes_wait()
    {
        const int ret = io_uring_submit_and_wait(&ring_, 1);
        check_syscall_return(ret);
        return ret;
    }

    void Worker::process_completions()
    {
        io_uring_cqe *cqe;

        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            const uint64_t op_id = io_uring_cqe_get_data64(cqe);

            if (op_id == kWakeupOpID)
            {
                ALOG_TRACE("Worker {} waked up", id_);
                // This was a wake-up event. Drain the task queue.
                std::coroutine_handle<> h;
                while (task_queue_.try_pop(h))
                {
                    h.resume();
                }

                // Only re-arm the wakeup read if not stopping
                // This prevents a pending operation during shutdown
                if (!stop_token_.stop_requested())
                {
                    submit_wakeup_read();
                }
                else
                {
                    ALOG_DEBUG("Worker {}: Not re-arming wakeup read (shutting down)", id_);
                }
                continue;
            }

            set_op_result(op_id, cqe->res);
            resume_coro_by_id(op_id);
            release_op_id(op_id);
        }
        io_uring_cq_advance(&ring_, count);
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

            std::abort();
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

        // worker is shutting down with non resumed coroutines
        if (op_data_pool_.size() != free_op_ids.size())
        {
            ALOG_WARN("Worker {} leaked {} op_ids during shutdown", id_, op_data_pool_.size() - free_op_ids.size());
        }

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

    void Worker::submit_wakeup_read()
    {
        io_uring_sqe *sqe = io_uring_get_sqe(&ring_);

        if (sqe == nullptr)
        {
            // SQ is full - force a submission to make room
            ALOG_WARN("Worker {}: SQ full, forcing submission to re-arm wakeup", id_);
            io_uring_submit(&ring_);

            // Try again
            sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                // Still full - this is CRITICAL
                ALOG_ERROR("Worker {}: Cannot submit wakeup read - SQ exhausted!", id_);
                ALOG_ERROR("Worker may not wake up on stop signal!");
                // This is a fatal, we cannot progress anymore
                throw std::runtime_error("Failed to submit wakeup read");
            }
        }

        io_uring_prep_read(sqe, wakeup_fd_, &wakeup_buffer_, sizeof(wakeup_buffer_), 0);
        io_uring_sqe_set_data64(sqe, kWakeupOpID);
        ALOG_TRACE("Worker {} submitted wakeup read SQE", id_);
    }

    void Worker::wakeup_write()
    {
        constexpr uint64_t value = 1;
        const auto ret = ::write(wakeup_fd_, &value, sizeof(value));
        if (ret != sizeof(value))
        {
            ALOG_ERROR("Failed to write to wakeup eventfd: {}", strerror(errno));
        }
        check_syscall_return(static_cast<int>(ret));
        ALOG_TRACE("Worker {} wrote to wakeup fd", id_);
    }

    void Worker::post(std::coroutine_handle<> h)
    {
        if (!task_queue_.try_push(h))
        {
            ALOG_ERROR("Worker {} task queue is full. Dropping task.", id_);
            // TODO: have a different strategy,
            // like growing the queue or blocking the producer.
            return;
        }

        // Signal the worker to wake up
        constexpr uint64_t val = 1;
        if (const ssize_t ret = ::write(wakeup_fd_, &val, sizeof(val)); ret < 0 && errno != EAGAIN)
        {
            ALOG_WARN("Worker {}: Failed to write to eventfd: {}", id_, strerror(errno));
        }
    }

    bool Worker::is_on_worker_thread() const { return std::this_thread::get_id() == thread_id_; }

    Task<Result<int>> Worker::async_accept(int server_fd, sockaddr *addr, socklen_t *addrlen)
    {
        auto prep = [](io_uring_sqe *sqe, const int fd, sockaddr *a, socklen_t *al, const int flags) { io_uring_prep_accept(sqe, fd, a, al, flags); };
        int ret = co_await make_uring_awaitable(*this, prep, server_fd, addr, addrlen, 0);
        if (ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }

        stats_.connections_accepted_total++;

        co_return ret;
    }

    Task<Result<int>> Worker::async_read(const int client_fd, std::span<char> buf) { co_return co_await async_read_at(client_fd, buf, static_cast<uint64_t>(-1)); }

    Task<Result<int>> Worker::async_read_at(const int client_fd, std::span<char> buf, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe *sqe, const int fd, char *b, const size_t len, const uint64_t off) { io_uring_prep_read(sqe, fd, b, static_cast<int>(len), off); };
        int ret = co_await make_uring_awaitable(*this, prep, client_fd, buf.data(), buf.size(), offset);
        if (ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }

        stats_.bytes_read_total += static_cast<uint64_t>(ret);
        stats_.read_ops_total++;

        co_return ret;
    }

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

    Task<Result<int>> Worker::async_write(const int client_fd, std::span<const char> buf) { co_return co_await async_write_at(client_fd, buf, static_cast<uint64_t>(-1)); }

    Task<Result<int>> Worker::async_write_at(const int client_fd, std::span<const char> buf, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe *sqe, const int fd, const char *b, const size_t len, const uint64_t off) { io_uring_prep_write(sqe, fd, b, static_cast<int>(len), off); };
        int ret = co_await make_uring_awaitable(*this, prep, client_fd, buf.data(), buf.size(), offset);
        if (ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }

        stats_.bytes_written_total += static_cast<uint64_t>(ret);
        stats_.write_ops_total++;

        co_return ret;
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

            int bytes_written = KIO_TRY(co_await this->async_write_at(client_fd, remaining_buf, current_offset));

            if (bytes_written == 0)
            {
                // EOF
                co_return std::unexpected(Error{ErrorCategory::File, kIoEof});
            }

            total_bytes_written += static_cast<size_t>(bytes_written);
        }

        co_return {};
    }

    Task<Result<int>> Worker::async_readv(const int client_fd, const iovec *iov, int iovcnt, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe *sqe, const int fd, const iovec *iov_, const int iovcnt_, const uint64_t off) { io_uring_prep_readv(sqe, fd, iov_, iovcnt_, off); };
        int ret = co_await make_uring_awaitable(*this, prep, client_fd, iov, iovcnt, offset);
        if (ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }

        stats_.bytes_read_total += static_cast<uint64_t>(ret);
        stats_.read_ops_total++;

        co_return ret;
    }

    Task<Result<int>> Worker::async_writev(const int client_fd, const iovec *iov, int iovcnt, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe *sqe, const int fd, const iovec *iov_, const int iovcnt_, const uint64_t off) { io_uring_prep_writev(sqe, fd, iov_, iovcnt_, off); };
        int ret = co_await make_uring_awaitable(*this, prep, client_fd, iov, iovcnt, offset);
        if (ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }

        stats_.bytes_written_total += static_cast<uint64_t>(ret);
        stats_.write_ops_total++;

        co_return ret;
    }

    Task<Result<int>> Worker::async_connect(const int client_fd, const sockaddr *addr, const socklen_t addrlen)
    {
        auto prep = [](io_uring_sqe *sqe, const int fd, const sockaddr *a, const socklen_t al) { io_uring_prep_connect(sqe, fd, a, al); };
        auto ret = co_await make_uring_awaitable(*this, prep, client_fd, addr, addrlen);
        if (ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }

        stats_.connect_ops_total++;

        co_return ret;
    }

    Task<Result<int>> Worker::async_openat(const std::filesystem::path path, const int flags, const mode_t mode)  // NOLINT on path, we need a copy in the coroutine frame, so a reference won't cut it
    {
        auto prep = [](io_uring_sqe *sqe, const int dfd, const char *p, const int f, const mode_t m) { io_uring_prep_openat(sqe, dfd, p, f, m); };
        int ret = co_await make_uring_awaitable(*this, prep, AT_FDCWD, path.c_str(), flags, mode);
        if (ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }

        stats_.open_ops_total++;

        co_return ret;
    }

    Task<Result<void>> Worker::async_fallocate(int fd, int mode, off_t size)
    {
        auto prep = [](io_uring_sqe *sqe, const int file_fd, const int p_mode, const off_t offset, const off_t len) { io_uring_prep_fallocate(sqe, file_fd, p_mode, offset, len); };
        off_t offset = 0;
        if (const int ret = co_await make_uring_awaitable(*this, prep, fd, mode, offset, size); ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }
        co_return {};
    }

    Task<Result<void>> Worker::async_close(int fd)
    {
        auto prep = [](io_uring_sqe *sqe, const int file_fd) { io_uring_prep_close(sqe, file_fd); };
        if (const int ret = co_await make_uring_awaitable(*this, prep, fd); ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }
        co_return {};
    }

    Task<Result<void>> Worker::async_fsync(int fd)
    {
        auto prep = [](io_uring_sqe *sqe, const int file_fd) { io_uring_prep_fsync(sqe, file_fd, 0); };
        if (const int ret = co_await make_uring_awaitable(*this, prep, fd); ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }
        co_return {};
    }

    Task<Result<void>> Worker::async_fdatasync(int fd)
    {
        auto prep = [](io_uring_sqe *sqe, const int file_fd)
        {
            // Use the IORING_FSYNC_DATASYNC flag
            io_uring_prep_fsync(sqe, file_fd, IORING_FSYNC_DATASYNC);
        };
        if (const int ret = co_await make_uring_awaitable(*this, prep, fd); ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }
        co_return {};
    }

    Task<Result<void>> Worker::async_unlink_at(const int dirfd, const std::filesystem::path path, const int flags)  // NOLINT path should be passed as value
    {
        auto prep = [](io_uring_sqe *sqe, const int dfd, const char *p, const int f) { io_uring_prep_unlinkat(sqe, dfd, p, f); };
        if (const int ret = co_await make_uring_awaitable(*this, prep, dirfd, path.c_str(), flags); ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }
        co_return {};
    }

    Task<Result<void>> Worker::async_sleep(std::chrono::nanoseconds duration)
    {
        // We need a place to store the timespec for the duration of the operation.
        // Storing it on the coroutine's frame by making it a local variable is perfect.
        __kernel_timespec ts{};
        ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        ts.tv_nsec = (duration % std::chrono::seconds(1)).count();

        auto prep = [](io_uring_sqe *sqe, __kernel_timespec *t, unsigned flags)
        {
            // Prepare a timeout operation. This doesn't need a file descriptor.
            io_uring_prep_timeout(sqe, t, 0, flags);
        };

        // Await the timeout operation.
        // The timeout operation returns -ECANCELED if cancelled.
        // A *successful* timer expiration completes with res = -ETIME.
        // We must treat -ETIME as a success, not an error.
        if (const int res = co_await make_uring_awaitable(*this, prep, &ts, 0); res < 0 && res != -ETIME)
        {
            // This is a real error (e.g. the operation was cancelled)
            co_return std::unexpected(Error::from_errno(-res));
        }

        // Success (res was 0, or res was -ETIME)
        co_return {};
    }

    Task<Result<void>> Worker::async_ftruncate(int fd, off_t length)
    {
        auto prep = [](io_uring_sqe *sqe, int file_fd, const off_t off) { io_uring_prep_ftruncate(sqe, file_fd, off); };
        if (const int ret = co_await make_uring_awaitable(*this, prep, fd, length); ret < 0)
        {
            co_return std::unexpected(Error::from_errno(-ret));
        }
        co_return {};
    }

}  // namespace kio::io
