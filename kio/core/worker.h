//
// Created by Yao ACHI on 17/10/2025.
//

#ifndef KIO_WORKER_H
#define KIO_WORKER_H

#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <latch>
#include <liburing.h>
#include <memory>
#include <span>
#include <thread>

#include "coro.h"
#include "errors.h"
#include "kio/net/net.h"
#include "kio/net/socket.h"
#include "kio/sync/mpsc_queue.h"
#include "uring_awaitable.h"

namespace kio::io
{
    struct IoCompletion
    {
        std::coroutine_handle<> handle;
        int result{0};

        void complete(const int res)
        {
            result = res;
            if (handle) handle.resume();
        }
    };

    class Worker;

    namespace internal
    {
        /**
         * @brief Attorney for accessing Worker's private scheduling APIs.
         *
         *
         * @warning DO NOT USE DIRECTLY IN APPLICATION CODE.
         *
         * Use high-level abstractions instead, for instance:
         * - SwitchToWorker(worker) for context switching
         * - DetachedTask for fire-and-forget operations
         * - Task<T> for structured concurrency
         *
         * This is intentionally in the 'internal' namespace to signal that it's
         * not part of the public API, even though it's technically accessible.
         */
        struct WorkerAccess
        {
            /**
             * @brief Schedule a coroutine handle on the worker's event loop.
             * @param worker The worker to schedule on
             * @param h The coroutine handle to schedule
             */
            static void post(Worker& worker, std::coroutine_handle<> h);
            static io_uring& get_ring(Worker& worker) noexcept;
        };
    }  // namespace internal

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
        uint64_t active_coroutines = 0;
        uint64_t io_errors_total = 0;
    };


    struct WorkerConfig
    {
        size_t uring_queue_depth{1024};
        size_t uring_submit_batch_size{128};
        size_t tcp_backlog{128};
        uint8_t uring_submit_timeout_ms{100};
        int uring_default_flags = 0;
        size_t max_op_slots{1024 * 1024};
        size_t task_queue_capacity{1024};

        void check() const
        {
            if (uring_queue_depth == 0 || uring_submit_batch_size == 0 || tcp_backlog == 0)
            {
                throw std::invalid_argument("Invalid worker config");
            }
            if (task_queue_capacity == 0 || (task_queue_capacity & (task_queue_capacity - 1)) != 0)
            {
                throw std::invalid_argument("task_queue_capacity must be a power of 2.");
            }
        }
    };

    /**
     * IoUringAwaitable, c++20 coroutine awaiter, controls the suspension and awaiting of
     * the io_uring operations.
     */
    template<typename Prep, typename... Args>
    struct IoUringAwaitable
    {
        Worker& worker_;
        Prep io_uring_prep_;
        using OnSuccess = void (*)(Worker&, int);
        OnSuccess on_success_;
        std::tuple<Args...> args_;
        IoCompletion completion_;

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h)
        {
            assert(worker_.is_on_worker_thread() && "kio::async_* operation was called from the wrong thread.");

            completion_.handle = h;
            auto& ring = internal::WorkerAccess::get_ring(worker_);
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);

            if (sqe == nullptr)
            {
                completion_.result = -EAGAIN;
                return false;
            }

            std::apply([this, sqe]<typename... T>(T&&... unpacked_args) { io_uring_prep_(sqe, std::forward<T>(unpacked_args)...); }, std::move(args_));

            io_uring_sqe_set_data(sqe, &completion_);
            return true;
        }

        [[nodiscard]] Result<int> await_resume() const noexcept
        {
            if (completion_.result < 0)
            {
                return std::unexpected(Error::from_errno(-completion_.result));
            }
            if (on_success_)
            {
                on_success_(worker_, completion_.result);
            }
            return completion_.result;
        }

        explicit IoUringAwaitable(Worker& worker, Prep prep, OnSuccess on_success, Args... args) : worker_(worker), io_uring_prep_(std::move(prep)), on_success_(on_success), args_(args...) {}
    };

    template<typename Prep, typename... Args>
    auto make_uring_awaitable(Worker& worker, Prep&& prep, void (*on_success)(Worker&, int), Args&&... args)
    {
        return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(worker, std::forward<Prep>(prep), on_success, std::forward<Args>(args)...);
    }

    // Overload for no stats callback
    template<typename Prep, typename... Args>
    auto make_uring_awaitable(Worker& worker, Prep&& prep, Args&&... args)
    {
        return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(worker, std::forward<Prep>(prep), nullptr, std::forward<Args>(args)...);
    }


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
        friend struct internal::WorkerAccess;
        template<typename, typename...>
        friend struct IoUringAwaitable;

        io_uring ring_{};
        WorkerConfig config_;
        size_t id_{0};
        std::atomic<std::thread::id> thread_id_{};
        MPSCQueue<std::coroutine_handle<>> task_queue_;

        int wakeup_fd_{-1};
        IoCompletion wakeup_completion_{.handle = nullptr};
        uint64_t wakeup_buffer_{0};

        std::latch init_latch_{1};
        std::latch shutdown_latch_{1};
        std::stop_source stop_source_;
        std::stop_token stop_token_;
        std::atomic<bool> stopped_{false};
        std::atomic<bool> wakeup_pending_{false};
        WorkerStats stats_{};
        std::function<void(Worker&)> worker_init_callback_;

        //=====================================
        // PRIVATE METHODS
        //=====================================
        int submit_sqes_wait();

        // Private Stats Updaters
        static void stat_inc_read(Worker& w, const int res)
        {
            w.stats_.bytes_read_total += res;
            w.stats_.read_ops_total++;
        }
        static void stat_inc_write(Worker& w, const int res)
        {
            w.stats_.bytes_written_total += res;
            w.stats_.write_ops_total++;
        }
        static void stat_inc_accept(Worker& w, int) { w.stats_.connections_accepted_total++; }
        static void stat_inc_connect(Worker& w, int) { w.stats_.connect_ops_total++; }
        static void stat_inc_open(Worker& w, int) { w.stats_.open_ops_total++; }

        static void check_kernel_version();
        void check_syscall_return(int ret);
        unsigned process_completions();
        // typically used during shutdown. Drain the completion queue
        void drain_completions();
        // used to wake the io uring processing loop up
        bool submit_wakeup_read();
        void wakeup_write() const;

        void post(std::coroutine_handle<> h);
        io_uring& get_ring() noexcept { return ring_; }
        void handle_cqe(io_uring_cqe* cqe);

    public:
        [[nodiscard]] bool is_on_worker_thread() const;
        void signal_init_complete() { init_latch_.count_down(); }
        void signal_shutdown_complete() { shutdown_latch_.count_down(); }
        [[nodiscard]] std::stop_token get_stop_token() const { return stop_source_.get_token(); }
        [[nodiscard]] size_t get_id() const noexcept { return id_; }
        void loop_forever();
        void wait_ready() const { init_latch_.wait(); }
        void wait_shutdown() const { shutdown_latch_.wait(); }
        [[nodiscard]] bool request_stop();
        void initialize();
        void loop();
        [[nodiscard]] bool is_running() const noexcept { return stopped_.load(std::memory_order_acquire) == false; }

        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
        Worker(Worker&&) = delete;
        Worker& operator=(Worker&&) = delete;
        Worker(size_t id, const WorkerConfig& config, std::function<void(Worker&)> worker_init_callback = {});
        ~Worker();

        [[nodiscard]] const WorkerStats& get_stats() { return stats_; }

        //==========================================================
        // Io methods
        //=========================================================

        [[nodiscard]] auto async_accept(int server_fd, sockaddr* addr, socklen_t* addrlen)
        {
            auto prep = [](io_uring_sqe* sqe, int fd, sockaddr* a, socklen_t* al, int flags) { io_uring_prep_accept(sqe, fd, a, al, flags); };
            return make_uring_awaitable(*this, prep, &Worker::stat_inc_accept, server_fd, addr, addrlen, 0);
        }

        [[nodiscard]] auto async_accept(const net::Socket& socket, net::SocketAddress& addr) { return async_accept(socket.get(), reinterpret_cast<sockaddr*>(&addr.addr), &addr.addrlen); }

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
        [[nodiscard]] auto async_read_at(int client_fd, std::span<char> buf, uint64_t offset)
        {
            auto prep = [](io_uring_sqe* sqe, const int fd, char* b, const unsigned len, const uint64_t off) { io_uring_prep_read(sqe, fd, b, len, off); };
            return make_uring_awaitable(*this, prep, &Worker::stat_inc_read, client_fd, buf.data(), static_cast<unsigned>(buf.size()), offset);
        }

        /**
         * @brief Asynchronously reads data from a streaming file descriptor (e.g., socket, pipe).
         *
         * @note This is for non-seekable I/O. It reads from the FD's current offset.
         *
         * @param client_fd The file descriptor to read from.
         * @param buf A span of memory to read data into.
         * @return An IO result with the client FD or an error
         */
        [[nodiscard]] auto async_read(const int client_fd, std::span<char> buf) { return async_read_at(client_fd, buf, static_cast<uint64_t>(-1)); }

        /**
         * @brief Asynchronously writes data to a positional file descriptor (e.g., file).
         *
         * @note This is for positional I/O. It writes at the FD's specified offset. It can be used with streaming
         * too by specifying -1 or 0 as an offset, but we suggest the dedicated method.
         *
         * @param fd The file descriptor to write to.
         * @param buf A span of constant memory to write.
         * @param offset
         * @return A void IO result or an error
         */
        [[nodiscard]] auto async_write_at(const int fd, std::span<const char> buf, const uint64_t offset)
        {
            auto prep = [](io_uring_sqe* sqe, const int fd, const char* b, const unsigned len, const uint64_t off) { io_uring_prep_write(sqe, fd, b, len, off); };
            return make_uring_awaitable(*this, prep, &Worker::stat_inc_write, fd, buf.data(), static_cast<unsigned>(buf.size()), offset);
        }

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
        [[nodiscard]] auto async_readv(int client_fd, const iovec* iov, int iovcnt, uint64_t offset)
        {
            auto prep = [](io_uring_sqe* sqe, int fd, const iovec* iov_, int iovcnt_, uint64_t off) { io_uring_prep_readv(sqe, fd, iov_, iovcnt_, off); };
            return make_uring_awaitable(*this, prep, &Worker::stat_inc_read, client_fd, iov, iovcnt, offset);
        }

        /**
         * @brief Asynchronously writes data to a streaming file descriptor (e.g., socket, pipe).
         *
         * @note This is for non-positional I/O. It writes to the FD's current offset.
         *
         * @param client_fd The file descriptor to write to.
         * @param buf A span of constant memory to write.
         * @return An IO result containing number of bytes writen FD or an error.
         */
        [[nodiscard]] auto async_write(int client_fd, std::span<const char> buf) { return async_write_at(client_fd, buf, static_cast<uint64_t>(-1)); }

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
        [[nodiscard]] auto async_writev(int client_fd, const iovec* iov, int iovcnt, uint64_t offset)
        {
            auto prep = [](io_uring_sqe* sqe, const int fd, const iovec* iov_, int iovcnt_, const uint64_t off) { io_uring_prep_writev(sqe, fd, iov_, iovcnt_, off); };
            return make_uring_awaitable(*this, prep, &Worker::stat_inc_write, client_fd, iov, iovcnt, offset);
        }

        /**
         * @brief Asynchronously initiates a connection on a socket.
         *
         * This is used for non-blocking client sockets.
         *
         * @param client_fd The socket file descriptor to connect.
         * @param addr A pointer to the sockaddr structure containing the peer address.
         * @param addrlen The length of the sockaddr structure.
         * @return A Task that resumes with a Result<int>.
         * On failure: An Error.
         */
        [[nodiscard]] auto async_connect(int client_fd, const sockaddr* addr, socklen_t addrlen)
        {
            auto prep = [](io_uring_sqe* sqe, const int fd, const sockaddr* a, const socklen_t al) { io_uring_prep_connect(sqe, fd, a, al); };
            return make_uring_awaitable(*this, prep, &Worker::stat_inc_connect, client_fd, addr, addrlen);
        }

        /**
         * @brief Asynchronously closes a file descriptor.
         *
         * After `co_await`ing this operation, the file descriptor `fd` must be
         * considered invalid, regardless of the operation's success.
         *
         * @param fd The file descriptor to close.
         * @return A void Result or an error.
         */
        [[nodiscard]] auto async_close(int fd)
        {
            auto prep = [](io_uring_sqe* sqe, int file_fd) { io_uring_prep_close(sqe, file_fd); };
            return make_uring_awaitable(*this, prep, fd);
        }

        /**
         * @brief Asynchronously flushes all modified data and metadata to disk.
         * Equivalent to fsync(2).
         * @param fd The file descriptor to sync.
         * @return A void Result or an error.
         */
        [[nodiscard]] auto async_fsync(int fd)
        {
            auto prep = [](io_uring_sqe* sqe, const int file_fd) { io_uring_prep_fsync(sqe, file_fd, 0); };
            return make_uring_awaitable(*this, prep, fd);
        }

        /**
         * @brief Asynchronously flushes all modified data to disk.
         * Equivalent to fdatasync(2). May not update metadata (like mtime).
         * @param fd The file descriptor to sync.
         * @return A void Result or an error.
         */
        [[nodiscard]] auto async_fdatasync(int fd)
        {
            auto prep = [](io_uring_sqe* sqe, const int file_fd) { io_uring_prep_fsync(sqe, file_fd, IORING_FSYNC_DATASYNC); };
            return make_uring_awaitable(*this, prep, fd);
        }

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
        [[nodiscard]] auto async_fallocate(int fd, int mode, off_t size)
        {
            auto prep = [](io_uring_sqe* sqe, int file_fd, int p_mode, off_t offset, off_t len) { io_uring_prep_fallocate(sqe, file_fd, p_mode, offset, len); };
            return make_uring_awaitable(*this, prep, fd, mode, static_cast<off_t>(0), size);
        }

        /**
         * @brief Truncates a file.
         *
         * @param fd FD of the file to truncate.
         * @param length Size of the truncate.
         * @return
         */
        [[nodiscard]] auto async_ftruncate(int fd, off_t length)
        {
            auto prep = [](io_uring_sqe* sqe, int file_fd, off_t off) { io_uring_prep_ftruncate(sqe, file_fd, off); };
            return make_uring_awaitable(*this, prep, fd, length);
        }

        [[nodiscard]] auto async_poll(int fd, int events)
        {
            auto prep = [](io_uring_sqe* sqe, int p_fd, int p_events) { io_uring_prep_poll_add(sqe, p_fd, static_cast<unsigned>(p_events)); };
            return make_uring_awaitable(*this, prep, fd, events);
        }

        [[nodiscard]] auto async_sendmsg(int fd, const msghdr* msg, int flags)
        {
            auto prep = [](io_uring_sqe* sqe, int f, const msghdr* m, int fl) { io_uring_prep_sendmsg(sqe, f, m, fl); };
            return make_uring_awaitable(*this, prep, &Worker::stat_inc_write, fd, msg, flags);
        }

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
        [[nodiscard]] auto async_openat(std::filesystem::path path, int flags, mode_t mode)
        {
            auto prep = [](io_uring_sqe* sqe, int dfd, const std::filesystem::path& p, int f, mode_t m) { io_uring_prep_openat(sqe, dfd, p.c_str(), f, m); };
            return make_uring_awaitable(*this, prep, &Worker::stat_inc_open, AT_FDCWD, std::move(path), flags, mode);
        }

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
        [[nodiscard]] auto async_unlink_at(int dirfd, std::filesystem::path path, int flags)
        {
            auto prep = [](io_uring_sqe* sqe, int dfd, const std::filesystem::path& p, int f) { io_uring_prep_unlinkat(sqe, dfd, p.c_str(), f); };
            return make_uring_awaitable(*this, prep, dirfd, std::move(path), flags);
        }

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
        Task<Result<void>> async_write_exact_at(int client_fd, std::span<const char> buf, uint64_t offset);


        /**
         * Asynchronously sleep for `duration`. This is a non-blocking sleep.
         * @param duration
         * @return void or an error
         */
        Task<std::expected<void, Error>> async_sleep(std::chrono::nanoseconds duration);


        /**
         * @brief Asynchronously transfers data between two file descriptors using a zero-copy pipe buffer.
         *
         * This function implements a generic "splice loop" to move data from a source to a destination
         * entirely within kernel space, avoiding userspace memory copies.
         *
         * Implementation Mechanics:
         * Linux `splice(2)` requires at least one of the file descriptors to be a pipe. To transfer
         * data between two non-pipe FDs (e.g., File->Socket or File->File), this function creates
         * a temporary local pipe to act as a kernel-space bridge:
         * [Source FD] --splice--> [Local Pipe] --splice--> [Destination FD]
         *
         * This double-splice technique ensures compatibility with any FD type supported by splice.
         *
         * Use Cases:
         * - **Network Streaming**: Sending files to TCP sockets (supports KTLS offload if enabled).
         * - **File Copying**: Efficiently copying data between files on disk.
         *
         * @param out_fd The destination file descriptor (Socket, File, etc.).
         * @param in_fd The source file descriptor (must support splice read).
         * @param offset is The offset in the source file to start reading from.
         * @param count  The total number of bytes to transfer.
         * @return void on success, or an Error on failure.
         */
        Task<Result<void>> async_sendfile(int out_fd, int in_fd, off_t offset, size_t count);
    };

    //==========================================================
    // Worker context switch
    //=========================================================
    /**
     * @brief An awaitable that switches the execution of a coroutine
     * to the thread of the specified IOWorker.
     */
    struct SwitchToWorker
    {
        Worker& worker_;

        explicit SwitchToWorker(Worker& worker) : worker_(worker) {}

        bool await_ready() const noexcept { return worker_.is_on_worker_thread(); }  // NOLINT
        void await_suspend(std::coroutine_handle<> h) const { internal::WorkerAccess::post(worker_, h); }
        void await_resume() const noexcept {}  // NOLINT
    };

    //
    // INTERNAL METHODS IMPLEMENTATION
    //
    namespace internal
    {
        inline void WorkerAccess::post(Worker& worker, std::coroutine_handle<> h) { worker.post(h); }

        inline io_uring& WorkerAccess::get_ring(Worker& worker) noexcept { return worker.get_ring(); }
    }  // namespace internal

}  // namespace kio::io

#endif  // KIO_WORKER_H
