//
// Created by ynachi on 10/3/25.
//

#include "../include/io_pool.h"

#include <sys/utsname.h>

#include "spdlog/spdlog.h"

namespace kio {

    void IOWorker::check_kernel_version()
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

    void IOWorker::check_syscall_return(const int ret) {
        if (ret >= 0) return;

        if (-ret == ETIME || -ret == ETIMEDOUT ||
            -ret == EINTR || -ret == EAGAIN || -ret == EBUSY) {
            spdlog::debug("Transient error: {}", strerror(-ret));
            return;
            }

        spdlog::error("Fatal I/O error: {}", strerror(-ret));
        throw std::system_error(-ret, std::system_category());
    }

    [[nodiscard]]
    uint64_t IOWorker::get_op_id() {
        if (!free_op_ids.empty()) {
            const auto id = free_op_ids.back();
            free_op_ids.pop_back();
            return id;
        }
        // TODO: manage the growth better
        // TODO: add metric for pool growth here
        // TODO: Log here
        // Grow pool (simple strategy: double capacity)
        if (op_data_pool_.size() >= config_.max_op_slots) {
            spdlog::error("Op slot pool exhausted ({} ops). Consider increasing default_op_slots.", op_data_pool_.size());
            throw std::runtime_error("Op slot pool exhausted");
        }

        const auto id = op_data_pool_.size();
        op_data_pool_.emplace_back();
        spdlog::debug("Growing op_data_pool_ to {}", op_data_pool_.size());
        return id;
    }

    void IOWorker::release_op_id(const uint64_t op_id) noexcept {
        if (op_id < op_data_pool_.size()) {
            op_data_pool_[op_id] = {};
            free_op_ids.push_back(op_id);
        } else {
            spdlog::warn("Tried to release invalid op_id={}", op_id);
        }
    }

     IOWorker::IOWorker(
         const size_t id,
         const IoWorkerConfig &config,
         std::shared_ptr<std::latch> init_latch,
         std::shared_ptr<std::latch> shutdown_latch):
    config_(config), id_(id), init_latch_(std::move(init_latch)), shutdown_latch_(std::move(shutdown_latch))
    {
        config.check();
        check_kernel_version();

        io_uring_params params{};
        params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

        if (const int ret = io_uring_queue_init_params(config_.uring_queue_depth, &ring_, &params); ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init failed");
        }

        op_data_pool_.resize(config_.default_op_slots);
        free_op_ids.reserve(config_.default_op_slots);
        // All slots start as free
        for (size_t i = 0; i < config_.default_op_slots; ++i) {
            free_op_ids.push_back(i);
        }
        spdlog::info("IO uring initialized with queue size of {}", config_.uring_queue_depth);
    }

    int IOWorker::submit_sqes_wait()
    {
        __kernel_timespec timeout = {.tv_sec = 0, .tv_nsec = config_.uring_submit_timeout_ms * 1000000};
        io_uring_cqe* cqe = nullptr;
        const int ret = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &timeout, nullptr);
        check_syscall_return(ret);
        return ret;
    }

    void IOWorker::process_completions()
    {
        io_uring_cqe* cqe;

        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            count++;
            const uint64_t op_id = io_uring_cqe_get_data64(cqe);

            if (op_id == ioWorkerStopSentinel)
            {
                spdlog::debug("stop signal received — shutting down");
                running_ = false;
                break;
            }

            set_op_result(op_id, cqe->res);
            resume_coro_by_id(op_id);
            release_op_id(op_id);
        }
        io_uring_cq_advance(&ring_, count);
    }

    void IOWorker::drain_completions()
    {
        process_completions();
    }


    void IOWorker::event_loop(const std::stop_token& stoken)
    {
        // 1. CPU Pinning - do this FIRST before anything else
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

        // 2. io-wq affinity (pin io_uring's kernel worker threads)
        if (const int ret = io_uring_register_iowq_aff(&ring_, 1, &cpuset); ret < 0)
        {
            spdlog::warn("Worker {}: Failed to set io-wq affinity: {}", id_, strerror(-ret));
        }
        else
        {
            spdlog::info("Worker {}: io-wq threads pinned to CPU {}", id_, cpu);
        }

        spdlog::info("Worker {} starting event loop", id_);
        running_ = true;

        while (running_ && !stoken.stop_requested())
        {
            // Submit pending operations
            // TODO: find a way to not pay runtime cost for  this if check, as submit_wait takes care of checking errors already
            if (auto sqe_num = submit_sqes_wait(); sqe_num > 0)
            {
                spdlog::trace("Worker {} submitted {} operations", id_, sqe_num);
                // TODO: place for metrics
            }

            // Process completions we got
            process_completions();
        }

        spdlog::info("Worker {} exiting event loop", id_);

        // Drain remaining completions
        drain_completions();

        // If we reach here, we are shutting down
        shutdown_latch_->count_down();

    }

    void IOWorker::request_stop()
    {
        if (auto* sqe = io_uring_get_sqe(&ring_)) {
            io_uring_prep_nop(sqe);
            io_uring_sqe_set_data64(sqe, ioWorkerStopSentinel);
            io_uring_submit(&ring_);
        }
        else
        {
            spdlog::error("failed to request stop");
            // TODO: retry
        }
    }

    IOWorker::~IOWorker()
    {
        spdlog::debug("Worker {} destructor called", id_);
        if (this->ring_.ring_fd >= 0)
        {
            io_uring_queue_exit(&ring_);
        }
    }

    Task<int> IOWorker::async_accept(int server_fd, sockaddr* addr, socklen_t* addrlen) {
        auto prep = [](io_uring_sqe* sqe, const int fd, sockaddr* a, socklen_t* al, const int flags) {
            io_uring_prep_accept(sqe, fd, a, al, flags);
        };

        co_return co_await make_uring_awaitable(*this, prep, server_fd, addr, addrlen, 0);
    }

    Task<int> IOWorker::async_read(const int client_fd, std::span<char> buf, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, char* b, const size_t len, const uint64_t off) {
            io_uring_prep_read(sqe, fd, b, len, off);
        };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, buf.data(), buf.size(), offset);
    }

    Task<int> IOWorker::async_write(const int client_fd, std::span<const char> buf, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const char* b, const size_t len, const uint64_t off) {
            io_uring_prep_write(sqe, fd, b, len, off);
        };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, buf.data(), buf.size(), offset);
    }

    Task<int> IOWorker::async_readv(const int client_fd, const iovec* iov, int iovcnt, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const iovec* iov, const int iovcnt, const uint64_t off) {
            io_uring_prep_readv(sqe, fd, iov, iovcnt, off);
        };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, iov, iovcnt, offset);
    }

    Task<int> IOWorker::async_writev(const int client_fd, const iovec* iov, int iovcnt, const uint64_t offset)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const iovec* iov, const int iovcnt, const uint64_t off) {
            io_uring_prep_writev(sqe, fd, iov, iovcnt, off);
        };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, iov, iovcnt, offset);
    }

    Task<int> IOWorker::async_connect(const int client_fd, const sockaddr* addr, const socklen_t addrlen)
    {
        auto prep = [](io_uring_sqe* sqe, const int fd, const sockaddr* a, const socklen_t al) {
            io_uring_prep_connect(sqe, fd, a, al);
        };
        co_return co_await make_uring_awaitable(*this, prep, client_fd, addr, addrlen);
    }

    Task<int> IOWorker::async_openat(std::string path, const int flags, const mode_t mode)
    {
        auto prep = [](io_uring_sqe* sqe, int dfd, const char* p, int f, mode_t m) {
            io_uring_prep_openat(sqe, dfd, p, f, m);
        };

        co_return co_await make_uring_awaitable(*this, prep, AT_FDCWD, path.c_str(), flags, mode);
    }

    Task<int> IOWorker::async_fallocate(int fd, int mode, off_t size)
    {
        auto prep = [](io_uring_sqe* sqe, int file_fd, int p_mode, off_t offset, off_t len) {
            io_uring_prep_fallocate(sqe, file_fd, p_mode, offset, len);
        };

        off_t offset = 0;
        co_return co_await make_uring_awaitable(*this, prep, fd, mode, offset, size);
    }

    Task<int> IOWorker::async_close(int fd)
    {
        auto prep = [](io_uring_sqe* sqe, int file_fd) {
            io_uring_prep_close(sqe, file_fd);
        };
        co_return co_await make_uring_awaitable(*this, prep, fd);
    }

     IOPool::IOPool(size_t num_workers, const IoWorkerConfig& config)
    : config_(config),
     init_latch_(std::make_shared<std::latch>(num_workers)),
     shutdown_latch_(std::make_shared<std::latch>(num_workers))
    {
        workers_.resize(num_workers, nullptr);

        for (size_t i = 0; i < num_workers; ++i) {
            worker_threads_.emplace_back([this, i](const std::stop_token& stoken) {
                try {
                    // Construct a worker on this thread
                    IOWorker worker(i, config_, init_latch_, shutdown_latch_);

                    // Store pointer for external access if needed
                    workers_[i] = &worker;

                    worker.signal_init_complete();

                    // Run event loop
                    worker.event_loop(stoken);

                } catch (const std::exception& e) {
                    spdlog::error("Worker {} failed: {}", i, e.what());
                    init_latch_->count_down();
                    shutdown_latch_->count_down();
                }
            });
        }

        // Wait for all workers to initialize
        init_latch_->wait();
        spdlog::info("IOPool started with {} workers", num_workers);
    }

    IOPool::IOPool(size_t num_workers, const IoWorkerConfig& config,
                const std::function<void(IOWorker&)>& worker_init)
     : config_(config),
       init_latch_(std::make_shared<std::latch>(num_workers)),
       shutdown_latch_(std::make_shared<std::latch>(num_workers))
    {
        for (size_t i = 0; i < num_workers; ++i) {
            worker_threads_.emplace_back([this, i, worker_init](const std::stop_token& stoken) {
                try {
                    // Construct worker on THIS thread
                    IOWorker worker(i, config_, init_latch_, shutdown_latch_);

                    worker.signal_init_complete();

                    // Call user's initialization callback
                    // This is where user starts their coroutines (accept_loop, etc.)
                    worker_init(worker);

                    // Run event loop
                    worker.event_loop(stoken);

                } catch (const std::exception& e) {
                    spdlog::error("Worker {} failed: {}", i, e.what());
                    init_latch_->count_down();
                    shutdown_latch_->count_down();
                }
            });
        }

        // Wait for all workers to initialize
        init_latch_->wait();
        spdlog::info("IOPool started with {} workers", num_workers);
    }
}
