//
// Created by ynachi on 10/3/25.
//

#ifndef KIO_IO_POOL_H
#define KIO_IO_POOL_H
#include <coroutine>
#include <latch>
#include <liburing.h>
#include <memory>
#include <vector>

#include "coro.h"

namespace kio {

    struct IoWorkerConfig
    {
        size_t uring_queue_depth{1024};
        size_t uring_submit_batch_size{128};
        size_t tcp_backlog{128};
        uint8_t uring_submit_timeout_ms{100};
        int uring_default_flags = 0;
        size_t default_op_slots{1024};
        float op_slots_growth_factor{1.5};
        size_t max_op_slots{1024 * 1024};

        constexpr IoWorkerConfig() = default;
        void check() const {
            if (uring_queue_depth == 0 || uring_submit_batch_size == 0 || tcp_backlog == 0 || op_slots_growth_factor <= 1.0f) {
                throw std::invalid_argument("Invalid worker config, some fields cannot be zero, or op_slots_growth_factor must be greater than 1.0f");
            }
        }
    };

    /**
     * An OI Worker is a self-contained event loop struct. It is not thread-safe and meant
     * to be used in a single thread setup only. Because we use the io uring single issuer
     * flag, the thread that constructs a worker instance must be the one submitting and
     * processing io requests. This is a tradeoff we made between API flexibility and
     * high horizontal scalability in a pure share-nothing manner. To see how to properly
     * set it up, you can look at the IOPool code.
     */
    class IOWorker {
        io_uring ring_{};
        IoWorkerConfig config_;
        size_t id_{0};

        struct io_op_data {
            std::coroutine_handle<> handle_;
            int result{-1};
        };

        std::vector<io_op_data> op_data_pool_;
        // max uint64 is reserved and should not be used as free op id
        std::vector<uint64_t> free_op_ids;

        std::shared_ptr<std::latch> init_latch_;
        std::shared_ptr<std::latch> shutdown_latch_;
        std::jthread thread_;
        // reading the stoken directly from the thread has been creating data races, so store a copy here
        std::stop_token stoken_;

        std::function<void(IOWorker&)> worker_init_callback_;
        void run(const std::stop_token& stoken);

        static void check_kernel_version();
        static void check_syscall_return(int ret);
        /**
         * submits current ready sqes to the ring, block until timeout if there is no submission.
         * When used in a loop (like an event loop), timing out allows checking other instructions.
         * @return The number of SQEs submitted or a negative errno.
         */
        int submit_sqes_wait();
        /**
         * Processes available completions from the ring since the lastest submit.
         * This method processes completions until a stop signal is received. Submissions
         * performed after the stop signal will not be processed.
         */
        void process_completions();
        // typically used during shutdown. Drain the completion queue
        void drain_completions();

    public:
        io_uring& get_ring() noexcept { return ring_; }
        [[nodiscard]]
        uint64_t get_op_id();
        void release_op_id(uint64_t op_id) noexcept;
        void init_op_slot(const uint64_t op_id, const std::coroutine_handle<> h) {
            op_data_pool_[op_id] = {h, 0};
        }
        [[nodiscard]]
        int64_t get_op_result(const uint64_t op_id) const
        {
            return op_data_pool_[op_id].result;
        }
        void set_op_result(const uint64_t op_id, const int result) {
            op_data_pool_[op_id].result = result;
        }
        void signal_init_complete() const {
            init_latch_->count_down();
        }
        void signal_shutdown_complete() const {
            shutdown_latch_->count_down();
        }
        void resume_coro_by_id(const uint64_t op_id) const
        {
            op_data_pool_[op_id].handle_.resume();
        }
        // sends a stop request signal
        void request_stop()
        {
            if (thread_.joinable())
            {
                // drain the completion queue
                drain_completions();
                // request stop if not already requested
                thread_.request_stop();
                // jthread joins on destruction automatically; but we ensure io_uring exit after thread finishes
            }
        };
        [[nodiscard]]
        std::stop_token get_stop_token() const noexcept
        {
            return stoken_;
        }
        [[nodiscard]]
        size_t get_id() const noexcept { return id_; }
        IOWorker(const IOWorker&) = delete;
        IOWorker& operator=(const IOWorker&) = delete;
        IOWorker(IOWorker&&) = delete;
        IOWorker& operator=(IOWorker&&) = delete;
        IOWorker(size_t id,
                 const IoWorkerConfig& config,
                 std::shared_ptr<std::latch> init_latch,
                 std::shared_ptr<std::latch> shutdown_latch,
                 std::function<void(IOWorker&)> worker_init_callback = {});
        ~IOWorker();

        // Io methods
        Task<int> async_accept(int server_fd, sockaddr* addr, socklen_t* addrlen);
        Task<int> async_read(int client_fd, std::span<char> buf, uint64_t offset);
        Task<int> async_openat(std::string_view path, int flags, mode_t mode);
        Task<int> async_write(int client_fd, std::span<const char> buf, uint64_t offset);
        Task<int> async_readv(int client_fd, const iovec* iov, int iovcnt, uint64_t offset);
        Task<int> async_writev(int client_fd, const iovec* iov, int iovcnt, uint64_t offset);
        Task<int> async_connect(int client_fd, const sockaddr* addr, socklen_t addrlen);
        Task<int> async_fallocate(int fd, int mode, off_t size);
        Task<int> async_close(int fd);
    };


    /**
 * IoUringAwaitable, c++20 coroutine awaiter, controls the suspension and awaiting of
 * the io_uring operations.
 */
    template<typename Prep, typename... Args>
    struct IoUringAwaitable
    {
        IOWorker& worker_;
        io_uring_sqe* sqe_ = nullptr;
        uint64_t op_id_{0};
        Prep io_uring_prep_;
        std::tuple<Args...> args_;
        int immediate_result_{0};

        bool await_ready() noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h)
        {
            sqe_ = io_uring_get_sqe(&worker_.get_ring());
            if (sqe_ == nullptr) {
                // no SQE available right now; return error immediately
                immediate_result_ = -EAGAIN;
                return false;
            }

            // allocate an op id and record the coroutine handle
            op_id_ = worker_.get_op_id();
            worker_.init_op_slot(op_id_, h);

            // Call prep sqe
            std::apply([this]<typename... T>(T&&... unpacked_args) {
                io_uring_prep_(sqe_, std::forward<T>(unpacked_args)...);
            }, args_);

            // save the op_id to the submission entry
            io_uring_sqe_set_data64(sqe_, op_id_);
            return true;
        }

        [[nodiscard]] int await_resume() const {
            if (immediate_result_ != 0) {
                return immediate_result_;
            }

            return static_cast<int>(worker_.get_op_result(op_id_));
        };

        explicit IoUringAwaitable(IOWorker& worker, Prep prep, Args... args)
            : worker_(worker), io_uring_prep_(std::move(prep)),
              args_(std::move(args)...)
        {}

    };

    template<typename Prep, typename... Args>
    auto make_uring_awaitable(IOWorker& worker, Prep&& prep, Args&&... args) {
        return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(
            worker,
            std::forward<Prep>(prep),
            std::forward<Args>(args)...
        );
    }

    /**
 * IOPool manages multiple IOWorker threads
 */
    class IOPool
    {
        std::vector<std::unique_ptr<IOWorker>> workers_;
        IoWorkerConfig config_;
        std::shared_ptr<std::latch> init_latch_;
        std::shared_ptr<std::latch> shutdown_latch_;

        public:
        explicit IOPool(size_t num_workers, const IoWorkerConfig& config = {});
        IOPool(size_t num_workers, const IoWorkerConfig& config, const std::function<void(IOWorker&)>& worker_init);
        ~IOPool() {
            shutdown_latch_->wait();
            spdlog::info("IOPool shutdown complete");
        }

        [[nodiscard]]
        size_t num_workers() const { return workers_.size(); }

        // Get worker by id (for submitting tasks to specific workers)
        [[nodiscard]]
        IOWorker* get_worker(const size_t id) const
        {
            if (id < workers_.size()) {
                return workers_[id].get();
            }
            return nullptr;
        }

        void stop()
        {
            for (const auto& worker : workers_) {
                worker->request_stop();
            }
            shutdown_latch_->wait();
        }

        size_t get_io_worker_id_by_key(std::string_view string);
    };
}

#endif //KIO_IO_POOL_H
