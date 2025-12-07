// Recommended Implementation: Hybrid Approach for KIO
// This combines the benefits of friend (true privacy) with internal namespace (clear intent)

// ============================================================================
// In worker.h
// ============================================================================

#ifndef KIO_WORKER_H
#define KIO_WORKER_H

namespace kio::io
{
    // Forward declarations
    class Worker;

    namespace internal
    {
        /**
         * @brief Internal access point for Worker's scheduling mechanism.
         *
         * This struct provides controlled access to Worker's private post() method
         * for coroutine infrastructure (SwitchToWorker, DetachedTask, etc.).
         *
         * @warning DO NOT USE DIRECTLY IN APPLICATION CODE.
         *
         * Use high-level abstractions instead:
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

            // Future extensions could go here:
            // static void post_urgent(Worker& worker, std::coroutine_handle<> h);
            // static void post_delayed(Worker& worker, std::coroutine_handle<> h, std::chrono::nanoseconds delay);
        };
    }  // namespace internal

    /**
     * @brief An I/O Worker is a self-contained event loop.
     *
     * Users interact with Worker through:
     * - async_* methods (async_read, async_write, etc.) - the primary API
     * - SwitchToWorker for context switching
     * - DetachedTask for fire-and-forget operations
     */
    class Worker
    {
        // Only the internal access helper can call post()
        friend struct internal::WorkerAccess;

        io_uring ring_{};
        WorkerConfig config_;
        size_t id_{0};
        std::atomic<std::thread::id> thread_id_{};

        // ... other private members ...

        /**
         * @brief Internal method to post a coroutine to this worker's task queue.
         *
         * This is private and should only be accessed via internal::WorkerAccess.
         * Direct use can lead to lifetime issues and race conditions.
         */
        void post(std::coroutine_handle<> h);

    public:
        // Constructor
        explicit Worker(size_t id, const WorkerConfig& config = {}, const std::function<void(Worker&)>& init_callback = {});

        // Lifecycle management
        void loop_forever();
        void wait_ready() const;
        void wait_shutdown() const;
        [[nodiscard]] bool request_stop();

        // Thread utilities
        [[nodiscard]] bool is_on_worker_thread() const;
        [[nodiscard]] std::stop_token get_stop_token() const;
        [[nodiscard]] size_t get_id() const noexcept;

        // Async I/O operations - THIS IS THE PUBLIC API
        Task<Result<int>> async_accept(int listen_fd, sockaddr* addr, socklen_t* addrlen);
        Task<Result<int>> async_read(int fd, std::span<char> buf);
        Task<Result<int>> async_write(int fd, std::span<const char> buf);
        Task<Result<void>> async_read_exact(int fd, std::span<char> buf);
        Task<Result<void>> async_write_exact(int fd, std::span<const char> buf);
        Task<Result<int>> async_connect(int fd, const sockaddr* addr, socklen_t addrlen);
        Task<Result<void>> async_close(int fd);
        Task<std::expected<void, Error>> async_sleep(std::chrono::nanoseconds duration);
        // ... etc ...
    };

    /**
     * @brief Awaitable that switches execution to a specific worker thread.
     *
     * Usage:
     *   Task<void> my_task(Worker& worker) {
     *       co_await SwitchToWorker(worker);
     *       // Now running on worker's thread
     *   }
     */
    struct SwitchToWorker
    {
        Worker& worker_;

        explicit SwitchToWorker(Worker& worker) : worker_(worker) {}

        bool await_ready() const noexcept { return worker_.is_on_worker_thread(); }

        void await_suspend(std::coroutine_handle<> h) const
        {
            internal::WorkerAccess::post(worker_, h);  // Uses internal access
        }

        void await_resume() const noexcept {}
    };

}  // namespace kio::io

#endif  // KIO_WORKER_H


// ============================================================================
// In worker.cpp - Implementation
// ============================================================================

#include "worker.h"

namespace kio::io
{
    void Worker::post(std::coroutine_handle<> h)
    {
        // Implementation of posting coroutine to task queue
        if (is_on_worker_thread())
        {
            // Direct resume if we're already on this worker
            h.resume();
        }
        else
        {
            // Cross-thread post to task queue
            task_queue_.push(h);
            wakeup_write();  // Wake up the worker event loop
        }
    }

    namespace internal
    {
        void WorkerAccess::post(Worker& worker, std::coroutine_handle<> h)
        {
            worker.post(h);  // Can access private method via friend
        }
    }  // namespace internal

    // ... rest of Worker implementation ...
}  // namespace kio::io


// ============================================================================
// In coro.h - DetachedTask also uses internal access
// ============================================================================

namespace kio
{
    struct DetachedTask
    {
        struct promise_type
        {
            DetachedTask get_return_object() noexcept { return DetachedTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }

            std::suspend_never initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() noexcept {}

            void unhandled_exception() noexcept
            {
                try
                {
                    throw;
                }
                catch (const std::exception& e)
                {
                    ALOG_ERROR("DetachedTask exception: {}", e.what());
                }
                catch (...)
                {
                    ALOG_ERROR("DetachedTask unknown exception");
                }
            }
        };

        using handle_t = std::coroutine_handle<promise_type>;
        handle_t coro;

        explicit DetachedTask(handle_t h) : coro(h) {}
        DetachedTask(DetachedTask&& o) noexcept : coro(std::exchange(o.coro, {})) {}

        ~DetachedTask()
        {
            if (coro) coro.destroy();
        }

        void detach() && noexcept
        {
            // Forget the handle → let coroutine run to completion
            coro = {};
        }
    };
}  // namespace kio


// ============================================================================
// Usage Example - Benchmark Code (CORRECT)
// ============================================================================

DetachedTask ping_pong_worker(Worker& worker, size_t conn_index)
{
    // Switch to worker thread - uses internal::WorkerAccess::post() internally
    co_await SwitchToWorker(worker);

    // Now safely running on worker thread
    auto addr = co_await parse_address(FLAGS_ip, FLAGS_port);
    auto sock = co_await create_socket(addr.family);
    // ... rest of implementation ...
}

int run_ping_pong_client()
{
    IOPool pool(FLAGS_vcpu_num, config);

    for (size_t i = 0; i < FLAGS_client_connection_num; ++i)
    {
        size_t worker_idx = i % FLAGS_vcpu_num;
        Worker* worker = pool.get_worker(worker_idx);
        if (worker)
        {
            // ✅ CORRECT: Detaches properly, no lifetime issues
            ping_pong_worker(*worker, i).detach();

            // ❌ WRONG: Can't do this anymore (won't compile)
            // worker->post(ping_pong_worker(*worker, i).h_);  // ERROR: post() is private
        }
    }

    // ... rest of implementation ...
}


// ============================================================================
// API Documentation Comment for README
// ============================================================================

/**
 * @section Threading Model
 *
 * KIO uses a single-threaded-per-worker model. Each Worker runs its own
 * io_uring event loop on a dedicated thread.
 *
 * To execute code on a specific worker, use SwitchToWorker:
 *
 * @code
 * Task<void> my_task(Worker& worker) {
 *     co_await SwitchToWorker(worker);
 *     // Now running on worker's thread
 *     auto result = co_await worker.async_read(fd, buffer);
 *     co_return;
 * }
 * @endcode
 *
 * For fire-and-forget operations, use DetachedTask:
 *
 * @code
 * DetachedTask handle_client(Worker& worker, int fd) {
 *     co_await SwitchToWorker(worker);
 *     // Handle client...
 * }
 *
 * // Spawn and forget
 * handle_client(worker, client_fd).detach();
 * @endcode
 *
 * @warning Do not directly manipulate coroutine handles or access
 * internal::WorkerAccess. These are implementation details.
 */
