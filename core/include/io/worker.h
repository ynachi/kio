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
#include "core/include/ds/mpsc_queue.h"

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

        // Task queue capacity (must be power of 2)
        // The queue will automatically round up to the next power of 2 if needed
        size_t task_queue_capacity{1024};

        constexpr WorkerConfig() = default;
        void check() const
        {
            if (uring_queue_depth == 0 || uring_submit_batch_size == 0 || tcp_backlog == 0 || op_slots_growth_factor <= 1.0f)
            {
                throw std::invalid_argument(
                        "Invalid worker config, some fields cannot be zero,"
                        " or op_slots_growth_factor must be greater than 1.0f");
            }

            if (task_queue_capacity == 0)
            {
                throw std::invalid_argument("task_queue_capacity must be > 0");
            }

            // Note: MPSCQueue constructor will assert if not power of 2,
            // but we can also check here for a better error message
            if ((task_queue_capacity & (task_queue_capacity - 1)) != 0)
            {
                throw std::invalid_argument(
                        "task_queue_capacity must be a power of 2. "
                        "Use next_power_of_2() helper if needed.");
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
        // Special op IDs for internal operations, starting from a high range
        // to avoid collision with user op_ids from the pool.
        static constexpr size_t kWakeupOpID = ~uint64_t{0};

        io_uring ring_{};
        WorkerConfig config_;
        size_t id_{0};
        // this is initialized when calling the loop, which can happen on
        // any thread.
        std::atomic<std::thread::id> thread_id_{};

        struct io_op_data
        {
            std::coroutine_handle<> handle_;
            int result{-1};
        };

        std::vector<io_op_data> op_data_pool_;
        // Lock-free queue for coroutines posted from other threads
        MPSCQueue<std::coroutine_handle<>> task_queue_;

        // max uint64 is reserved and should not be used as free op id
        std::vector<uint64_t> free_op_ids;
        // To immediately wake up the worker loop
        int wakeup_fd_{-1};
        uint64_t wakeup_buffer_{0};

        std::latch init_latch_{1};
        std::latch shutdown_latch_{1};
        std::stop_source stop_source_;
        // updated by all run method
        std::stop_token stop_token_;
        // to avoid double shutdown
        std::atomic<bool> stopped_{false};

        std::function<void(Worker&)> worker_init_callback_;

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
        // used to wake the io uring processing loop up
        void submit_wakeup_read();
        void wakeup_write();

    public:
        void post(std::coroutine_handle<> h);
        [[nodiscard]] bool is_on_worker_thread() const;

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

        void signal_init_complete() { init_latch_.count_down(); }
        void signal_shutdown_complete() { shutdown_latch_.count_down(); }
        void resume_coro_by_id(const uint64_t op_id) const { op_data_pool_[op_id].handle_.resume(); }
        [[nodiscard]]
        std::stop_token get_stop_token() const
        {
            return stop_source_.get_token();
        }
        // sends a stop request signal
        [[nodiscard]]
        size_t get_id() const noexcept
        {
            return id_;
        }
        void loop_forever();

        void wait_ready() const { init_latch_.wait(); }
        void wait_shutdown() const { shutdown_latch_.wait(); }
        [[nodiscard]]
        bool request_stop();

        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        Worker(Worker&&) = delete;
        Worker& operator=(Worker&&) = delete;
        Worker(size_t id, const WorkerConfig& config, std::function<void(Worker&)> worker_init_callback = {});
        void initialize();
        void loop();
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

    /**
     * @brief An awaitable that switches the execution of a coroutine
     * to the thread of the specified IOWorker.
     */
    struct SwitchToWorker
    {
        Worker& worker_;

        explicit SwitchToWorker(Worker& worker) : worker_(worker) {}

        bool await_ready() const noexcept { return worker_.is_on_worker_thread(); }
        void await_suspend(std::coroutine_handle<> h) const { worker_.post(h); }
        void await_resume() const noexcept {}
    };
}  // namespace kio::io

#endif  // KIO_WORKER_H
