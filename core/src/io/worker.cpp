//
// Created by Yao ACHI on 17/10/2025.
//
#include "core/include/io/worker.h"

#include <spdlog/spdlog.h>
#include <sys/utsname.h>

#include "core/include/io/uring_awaitable.h"

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
            spdlog::debug("Transient error: {}", strerror(-ret));
            return;
        }

        spdlog::error("Fatal I/O error: {}", strerror(-ret));
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
            spdlog::error("Op slot pool exhausted ({} ops). Consider increasing default_op_slots.", op_data_pool_.size());
            throw std::runtime_error("Op slot pool exhausted");
        }

        const auto cur_size = op_data_pool_.size();
        size_t new_size = std::min(static_cast<size_t>(static_cast<float>(cur_size) * config_.op_slots_growth_factor), config_.max_op_slots);
        spdlog::debug("Growing op_data_pool_ from {} to {}", cur_size, new_size);
        op_data_pool_.resize(new_size);
        for (size_t i = cur_size; i < new_size; ++i)
        {
            free_op_ids.push_back(i);
        }

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
            spdlog::warn("Tried to release invalid op_id={}", op_id);
        }
    }

    Worker::Worker(const size_t id, const WorkerConfig& config, std::function<void(Worker&)> worker_init_callback) : config_(config), id_(id), worker_init_callback_(std::move(worker_init_callback))
    {
        config_.check();

        // pre-allocate op pool metadata on the constructing thread to avoid allocation races
        op_data_pool_.resize(config_.default_op_slots);
        free_op_ids.reserve(config_.default_op_slots);
        for (size_t i = 0; i < config_.default_op_slots; ++i)
        {
            free_op_ids.push_back(i);
        }
    }

    // The main per-thread body that used to be inside the thread lambda.
    void Worker::loop_forever()
    {
        // the calling thread sets the id
        thread_id_ = std::this_thread::get_id();
        // save the stop token
        stop_token_ = this->get_stop_token();

        try
        {
            check_kernel_version();

            io_uring_params params{};
            params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

            if (const int ret = io_uring_queue_init_params(config_.uring_queue_depth, &ring_, &params); ret < 0)
            {
                throw std::system_error(-ret, std::system_category(), "io_uring_queue_init failed");
            }

            spdlog::info("IO uring initialized with queue size of {}", config_.uring_queue_depth);

            // After io_uring init, we can set CPU affinity and iowq affinity and then signal init complete
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            const uint cpu = id_ % std::thread::hardware_concurrency();
            CPU_SET(cpu, &cpuset);

            if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
            {
                spdlog::warn("Worker {}: Failed to set CPU affinity: {}", id_, strerror(rc));
            }
            else
            {
                spdlog::info("Worker {} pinned to CPU {}", id_, cpu);
            }

            if (const int ret = io_uring_register_iowq_aff(&ring_, 1, &cpuset); ret < 0)
            {
                spdlog::warn("Worker {}: Failed to set io-wq affinity: {}", id_, strerror(-ret));
            }
            else
            {
                spdlog::info("Worker {}: io-wq threads pinned to CPU {}", id_, cpu);
            }

            // Init callback to run inside the worker, respect io_uring single issuer
            if (worker_init_callback_)
            {
                worker_init_callback_(*this);
            }

            spdlog::info("Worker {} starting event loop", id_);

            // event loop (split out to reuse the same logic as before)
            while (!stop_token_.stop_requested())
            {
                if (auto sqe_num = submit_sqes_wait(); sqe_num > 0)
                {
                    spdlog::trace("Worker {} submitted {} operations", id_, sqe_num);
                }
                process_completions();
            }

            spdlog::info("Worker {} exiting event loop", id_);

            drain_completions();
        }
        catch (const std::exception& e)
        {
            spdlog::error("Worker {} failed during run(): {}", id_, e.what());
            // ensure latches are released so the pool doesn't wait forever
            this->wait_ready();
            this->wait_shutdown();
        }
    }

    int Worker::submit_sqes_wait()
    {
        __kernel_timespec timeout = {.tv_sec = 0, .tv_nsec = config_.uring_submit_timeout_ms * 1000000};
        io_uring_cqe* cqe = nullptr;
        const int ret = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &timeout, nullptr);
        check_syscall_return(ret);
        return ret;
    }

    void Worker::process_completions()
    {
        io_uring_cqe* cqe;

        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            const uint64_t op_id = io_uring_cqe_get_data64(cqe);

            set_op_result(op_id, cqe->res);
            resume_coro_by_id(op_id);
            release_op_id(op_id);
        }
        io_uring_cq_advance(&ring_, count);
    }

    void Worker::drain_completions() { process_completions(); }

    Worker::~Worker()
    {
        spdlog::debug("Worker {} destructor called", id_);
        // request stop if not already requested

        if (this->ring_.ring_fd >= 0)
        {
            io_uring_queue_exit(&ring_);
        }
    }

    Task<int> Worker::async_accept(int server_fd, sockaddr* addr, socklen_t* addrlen)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, sockaddr* a, socklen_t* al, const int flags) { io_uring_prep_accept(sqe, fd, a, al, flags); };

        co_return co_await make_uring_awaitable(*this, prep, server_fd, addr, addrlen, 0);
    }

    Task<int> Worker::async_read(const int client_fd, std::span<char> buf, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, char* b, const size_t len, const uint64_t off) { io_uring_prep_read(sqe, fd, b, len, off); };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, buf.data(), buf.size(), offset);
    }

    Task<int> Worker::async_write(const int client_fd, std::span<const char> buf, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const char* b, const size_t len, const uint64_t off) { io_uring_prep_write(sqe, fd, b, len, off); };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, buf.data(), buf.size(), offset);
    }

    Task<int> Worker::async_readv(const int client_fd, const iovec* iov, int iovcnt, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const iovec* iov, const int iovcnt, const uint64_t off) { io_uring_prep_readv(sqe, fd, iov, iovcnt, off); };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, iov, iovcnt, offset);
    }

    Task<int> Worker::async_writev(const int client_fd, const iovec* iov, int iovcnt, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const iovec* iov, const int iovcnt, const uint64_t off) { io_uring_prep_writev(sqe, fd, iov, iovcnt, off); };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, iov, iovcnt, offset);
    }

    Task<int> Worker::async_connect(const int client_fd, const sockaddr* addr, const socklen_t addrlen)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const sockaddr* a, const socklen_t al) { io_uring_prep_connect(sqe, fd, a, al); };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, addr, addrlen);
    }

    Task<int> Worker::async_openat(std::string_view path, const int flags, const mode_t mode)
    {
        // make the coroutine own its own copy of the path in its frame
        const std::string path_str(path);
        auto prep = [](io_uring_sqe* sqe, const int dfd, const char* p, const int f, const mode_t m) { io_uring_prep_openat(sqe, dfd, p, f, m); };

        co_return co_await make_uring_awaitable(*this, prep, AT_FDCWD, path_str.c_str(), flags, mode);
    }

    Task<int> Worker::async_fallocate(int fd, int mode, off_t size)
    {
        auto prep = [](io_uring_sqe* sqe, int file_fd, int p_mode, off_t offset, off_t len) { io_uring_prep_fallocate(sqe, file_fd, p_mode, offset, len); };

        off_t offset = 0;
        co_return co_await make_uring_awaitable(*this, prep, fd, mode, offset, size);
    }

    Task<int> Worker::async_close(int fd)
    {
        auto prep = [](io_uring_sqe* sqe, int file_fd) { io_uring_prep_close(sqe, file_fd); };
        co_return co_await make_uring_awaitable(*this, prep, fd);
    }

}  // namespace kio::io
