//
// Created by Yao ACHI on 17/10/2025.
//

#ifndef KIO_WORKER_H
#define KIO_WORKER_H

#include <expected>
#include <filesystem>
#include <latch>
#include <liburing.h>
#include <memory>
#include <span>
#include <thread>

#include "core/include/coro.h"
#include "core/include/ds/mpsc_queue.h"
#include "core/include/errors.h"

namespace kio::io
{
    struct alignas(std::hardware_destructive_interference_size) WorkerStats
    {
        uint64_t bytes_read_total = 0;
        uint64_t bytes_written_total = 0;
        uint64_t read_ops_total = 0;
        uint64_t write_ops_total = 0;
        uint64_t connections_accepted_total = 0;
        uint64_t open_ops_total = 0;
        uint64_t connect_ops_total = 0;
        uint64_t coroutines_pool_resize_total = 0;
        // This is also the amount of op id used from the pool
        uint64_t active_coroutines = 0;
    };


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
        // to avoid a double shutdown
        std::atomic<bool> stopped_{false};
        WorkerStats stats_{};

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

        // stats
        /**
         * @brief Gets this worker's non-atomic stats.
         * MUST only be called from this worker's thread.
         * (Or safely via the IOPoolCollector)
         */
        [[nodiscard]] const WorkerStats& get_stats()
        {
            stats_.active_coroutines = op_data_pool_.size() - free_op_ids.size();
            return stats_;
        }
        // Io methods
        Task<Result<int>> async_accept(int server_fd, sockaddr* addr, socklen_t* addrlen);
        /**
         * @brief Asynchronously reads data from a streaming file descriptor (e.g., socket, pipe).
         *
         * @note This is for non-seekable I/O. It reads from the FD's current offset.
         *
         * @param client_fd The file descriptor to read from.
         * @param buf A span of memory to read data into.
         * @return An IO result with the client FD or an error
         */
        Task<Result<int>> async_read(int client_fd, std::span<char> buf);
        /**
         * @brief Asynchronously reads data from a file descriptor at a specific offset.
         *
         * @note This is for positional I/O.
         *
         * @param client_fd The file descriptor to read from.
         * @param buf A span of memory to read data into.
         * @param offset The file offset to read from.
         * @return An IO Result embedding the number of bytes read or an error.
         */
        Task<Result<int>> async_read_at(int client_fd, std::span<char> buf, uint64_t offset);
        /**
         * @brief Asynchronously reads data from a file descriptor until the buffer is full.
         *
         * @note This function guarantees that it will not return successfully unless
         * `buf.size()` bytes have been read. It does this by repeatedly calling
         * the underlying `async_read` operation internally until the buffer is
         * filled, automatically handling "short reads". This variant is for non-positional FDs
         * i.e., streaming like networks, pipes, ...
         *
         * @param client_fd The file descriptor to read from.
         * @param buf A span of memory to be filled with data.
         * @return An IO Result embedding the number of bytes read or an error.
         */
        Task<Result<void>> async_read_exact(int client_fd, std::span<char> buf);
        /**
         * @brief Read the exact data size to fill the provided buffer. This method requires
         * the FD to be seekable.
         *
         * @note Reaching EOF before filling the buffer is considered an error.
         * In this case, a IoError::IoEoF will be returned.
         *
         * @param fd the FD to read from
         * @param buf a writable std span
         * @param offset the offset from which to read from
         * @return A void IO result or an error
         */
        Task<Result<void>> async_read_exact_at(int fd, std::span<char> buf, uint64_t offset);
        /**
         * @brief Asynchronously opens or creates a file.
         *
         * @note This implementation uses `AT_FDCWD` internally,
         * meaning the `path` is resolved relative to the current working directory.
         * The coroutine frame will store a copy of the path string.
         *
         * @param path The path to the file.
         * @param flags The file access flags (e.g., O_RDONLY, O_WRONLY, O_CREAT).
         * @param mode The file permissions mode (e.g., 0644) used only if O_CREAT is specified.
         * @return An IO Result which is void or an error.
         */
        Task<Result<int>> async_openat(std::filesystem::path path, int flags, mode_t mode);
        /**
         * @brief Asynchronously writes data to a streaming file descriptor (e.g., socket, pipe).
         *
         * @note This is for non-positional I/O. It writes to the FD's current offset.
         *
         * @param client_fd The file descriptor to write to.
         * @param buf A span of constant memory to write.
         * @return An IO result containing number of bytes writen FD or an error.
         */
        Task<Result<int>> async_write(int client_fd, std::span<const char> buf);
        /**
         * @brief Asynchronously writes data to a positional file descriptor (e.g., file).
         *
         * @note This is for positional I/O. It writes at the FD's specified offset. It can be used with streaming
         * too by specifying -1 or 0 as an offset, but we suggest the dedicated method.
         *
         * @param client_fd The file descriptor to write to.
         * @param buf A span of constant memory to write.
         * @return A void IO result or an error
         */
        Task<Result<int>> async_write_at(int client_fd, std::span<const char> buf, uint64_t offset);
        /**
         * @brief Asynchronously writes the entire contents of a buffer to a file descriptor.
         *
         * @note This function guarantees that it will not return successfully unless
         * the *entire* `buf.size()` bytes have been written. It does this by
         * repeatedly calling the underlying `async_write` operation internally
         * until the buffer is fully written, automatically handling "short writes".
         *
         * @param client_fd The file descriptor to write to.
         * @param buf A span of constant memory to be written in its entirety.
         * @return A void IO result or an error
         */
        Task<Result<void>> async_write_exact(int client_fd, std::span<const char> buf);
        /**
         * @brief Asynchronously reads data into a "scatter" array of buffers.
         *
         * @note This performs a *single* readv operation. It is not guaranteed to fill
         * all buffers.
         *
         * @param client_fd The file descriptor to read from.
         * @param iov A pointer to an array of iovec structures.
         * @param iovcnt The number of iovec structures in the array.
         * @param offset The file offset to read from.
         * @return An IO result of the number of bytes read or an error.
         */
        Task<Result<int>> async_readv(int client_fd, const iovec* iov, int iovcnt, uint64_t offset);
        Task<Result<void>> async_write_exact_at(int client_fd, std::span<const char> buf, uint64_t offset);
        /**
         * @brief Asynchronously writes data from a "gather" array of buffers.
         *
         * @note This performs a *single* writev operation. It is not guaranteed to
         * write the data from all buffers.
         *
         * @param client_fd The file descriptor to write to.
         * @param iov A pointer to an array of iovec structures.
         * @param iovcnt The number of iovec structures in the array.
         * @param offset The file offset to write at.
         * @return A Task that resumes with a Result<int>.
         * On success: The total number of bytes written from all buffers.
         * On failure: An Error.
         */
        Task<Result<int>> async_writev(int client_fd, const iovec* iov, int iovcnt, uint64_t offset);
        /**
         * @brief Asynchronously initiates a connection on a socket.
         *
         * This is used for non-blocking client sockets.
         *
         * @param client_fd The socket file descriptor to connect.
         * @param addr A pointer to the sockaddr structure containing the peer address.
         * @param addrlen The length of the sockaddr structure.
         * @return A Task that resumes with a Result<int>.
         * On success: 0.
         * On failure: An Error.
         */
        Task<Result<int>> async_connect(int client_fd, const sockaddr* addr, socklen_t addrlen);
        /**
         * @brief Asynchronously pre-allocates or de-allocates storage space for a file.
         *
         * @note This implementation internally calls `io_uring_prep_fallocate` with
         * the offset fixed at 0.
         *
         * @param fd The file descriptor.
         * @param mode The operation mode (e.g., 0 for falloc, FALLOC_FL_PUNCH_HOLE).
         * @param size The number of bytes to allocate (length).
         *  @return A void Result or an error.
         */
        Task<Result<void>> async_fallocate(int fd, int mode, off_t size);
        /**
         * @brief Asynchronously closes a file descriptor.
         *
         * After `co_await`ing this operation, the file descriptor `fd` must be
         * considered invalid, regardless of the operation's success.
         *
         * @param fd The file descriptor to close.
         * @return A void Result or an error.
         */
        Task<Result<void>> async_close(int fd);
        /**
         * Asynchronously sleep for `duration`. This is a non-blocking sleep.
         * @param duration
         * @return void or an error
         */
        Task<std::expected<void, Error>> async_sleep(std::chrono::nanoseconds duration);

        /**
         * @brief Asynchronously flushes all modified data and metadata to disk.
         * Equivalent to fsync(2).
         * @param fd The file descriptor to sync.
         * @return A void Result or an error.
         */
        Task<Result<void>> async_fsync(int fd);

        /**
         * @brief Asynchronously flushes all modified data to disk.
         * Equivalent to fdatasync(2). May not update metadata (like mtime).
         * @param fd The file descriptor to sync.
         * @return A void Result or an error.
         */
        Task<Result<void>> async_fdatasync(int fd);
        /**
         * @brief Asynchronously unlink (remove) a file or directory.
         *
         * @param dirfd The directory file descriptor (use AT_FDCWD for the current working directory).
         * @param path The filesystem path to remove.
         * @param flags Removal behavior flags:
         *             - 0: Remove regular files
         *             - AT_REMOVEDIR: Remove empty directories
         *             - AT_SYMLINK_NOFOLLOW: Remove symlinks without following
         * @return An IO Result which is void or an error.
         */
        Task<Result<void>> async_unlink_at(int dirfd, std::filesystem::path path, int flags);
        Task<Result<void>> async_ftruncate(int fd, off_t length);
    };

    /**
     * @brief An awaitable that switches the execution of a coroutine
     * to the thread of the specified IOWorker.
     */
    struct SwitchToWorker
    {
        Worker& worker_;

        explicit SwitchToWorker(Worker& worker) : worker_(worker) {}

        bool await_ready() const noexcept { return worker_.is_on_worker_thread(); }  // NOLINT
        void await_suspend(std::coroutine_handle<> h) const { worker_.post(h); }
        void await_resume() const noexcept {}  // NOLINT
    };
}  // namespace kio::io

#endif  // KIO_WORKER_H
