//
// Created by Yao ACHI on 17/10/2025.
//

#ifndef KIO_WORKER_H
#define KIO_WORKER_H

#include <latch>
#include <liburing.h>
#include <memory>
#include <span>
#include <string_view>
#include <thread>

#include "core/include/coro.h"

namespace kio::io
{
    struct WorkerConfig
    {
        size_t uring_queue_depth{1024};
        size_t uring_submit_batch_size{128};
        size_t tcp_backlog{128};
        uint8_t uring_submit_timeout_ms{100};
        int uring_default_flags = 0;
        size_t default_op_slots{1024};
        float op_slots_growth_factor{1.5};
        size_t max_op_slots{1024 * 1024};

        constexpr WorkerConfig() = default;
        void check() const
        {
            if (uring_queue_depth == 0 || uring_submit_batch_size == 0 || tcp_backlog == 0 || op_slots_growth_factor <= 1.0f)
            {
                throw std::invalid_argument("Invalid worker config, some fields cannot be zero, or op_slots_growth_factor must be greater than 1.0f");
            }
        }
    };

    /**
     * An IO Worker is a self-contained event loop struct.
     *
     * THREAD SAFETY:
     * - Safe to call async_* methods from ANY thread via submit_cross_thread()
     * - Direct async_* calls must only be made from the worker's own thread
     * - The worker internally uses io_uring SINGLE_ISSUER mode for maximum performance, trading off flexibility vs. performance.
     */
    class Worker
    {
        io_uring ring_{};
        WorkerConfig config_;
        size_t id_{0};
        std::thread::id thread_id_{};

        struct io_op_data
        {
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

        std::function<void(Worker&)> worker_init_callback_;
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
        void init_op_slot(const uint64_t op_id, const std::coroutine_handle<> h) { op_data_pool_[op_id] = {h, 0}; }
        [[nodiscard]]
        int64_t get_op_result(const uint64_t op_id) const
        {
            return op_data_pool_[op_id].result;
        }
        void set_op_result(const uint64_t op_id, const int result) { op_data_pool_[op_id].result = result; }
        void signal_init_complete() const { init_latch_->count_down(); }
        void signal_shutdown_complete() const { shutdown_latch_->count_down(); }
        void resume_coro_by_id(const uint64_t op_id) const { op_data_pool_[op_id].handle_.resume(); }
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
        size_t get_id() const noexcept
        {
            return id_;
        }
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        Worker(Worker&&) = delete;
        Worker& operator=(Worker&&) = delete;
        Worker(size_t id, const WorkerConfig& config, std::shared_ptr<std::latch> init_latch, std::shared_ptr<std::latch> shutdown_latch, std::function<void(Worker&)> worker_init_callback = {});
        ~Worker();

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
}  // namespace kio::io

#endif  // KIO_WORKER_H
