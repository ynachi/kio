//
// Created by ynachi on 10/3/25.
//

#ifndef KIO_IO_WORKER_POOL_H
#define KIO_IO_WORKER_POOL_H
#include <liburing.h>
#include <memory>
#include <vector>

#include "core/include/coro.h"
#include "worker.h"

namespace kio::io
{

    /**
     * @brief Manages a pool of I/O worker threads.
     *
     * This class is responsible for creating, managing, and tearing down a
     * collection of Worker threads. It provides a simple mechanism for distributing
     * work across the pool.
     *
     * LIFETIME: The IOPool instance MUST outlive any coroutines that interact
     * with its workers. This is a deliberate design choice to avoid the runtime
     * overhead of shared pointers in the hot path.
     */
    class IOPool
    {
        std::vector<std::unique_ptr<Worker>> workers_;
        std::vector<std::jthread> worker_threads_;

    public:
        /**
         * @brief Construct and start an IOPool.
         * @param num_workers The number of worker threads to create.
         * @param config Configuration for each worker.
         * @param worker_init An optional callback to run on each worker thread after initialization.
         */
        explicit IOPool(size_t num_workers, const WorkerConfig& config = {}, const std::function<void(Worker&)>& worker_init = {});

        ~IOPool();

        IOPool(const IOPool&) = delete;
        IOPool& operator=(const IOPool&) = delete;

        /**
         * @brief Get the number of workers in the pool.
         */
        [[nodiscard]]
        size_t num_workers() const
        {
            return workers_.size();
        }

        /**
         * @brief Get a pointer to a specific worker by its ID.
         * @return Pointer to the worker, or nullptr if the ID is invalid.
         */
        [[nodiscard]]
        Worker* get_worker(size_t id) const;

        /**
         * @brief Get a worker ID based on a hash of a key.
         *
         * This is useful for affinity, ensuring that operations for the same
         * resource (e.g., a file path or socket FD) are always handled by the
         * same worker thread.
         * @param key A string_view to hash for selecting a worker.
         * @return The ID of the selected worker.
         */
        [[nodiscard]]
        size_t get_worker_id_by_key(std::string_view key) const;

        /**
         * @brief Request all workers to stop and waits for them to shut down.
         */
        void stop();
    };
}  // namespace kio::io

#endif  // KIO_IO_WORKER_POOL_H
