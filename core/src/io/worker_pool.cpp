//
// Created by Yao ACHI on 19/10/2025.
//
#include "core/include/io/worker_pool.h"

namespace kio::io
{
    IOPool::IOPool(size_t num_workers, const WorkerConfig& config, const std::function<void(Worker&)>& worker_init)
    {
        if (num_workers == 0)
        {
            throw std::invalid_argument("IOPool must have at least one worker.");
        }

        workers_.reserve(num_workers);
        worker_threads_.reserve(num_workers);

        for (size_t i = 0; i < num_workers; ++i)
        {
            workers_.emplace_back(std::make_unique<Worker>(i, config, worker_init));
        }

        // Start threads after all workers are created
        for (const auto& worker: workers_)
        {
            worker_threads_.emplace_back([&w = *worker] { w.loop_forever(); });
        }

        // Wait for all workers to be fully initialized
        for (const auto& worker: workers_)
        {
            worker->wait_ready();
        }

        spdlog::info("IOPool started with {} workers", num_workers);
    }

    IOPool::~IOPool()
    {
        // The `stop()` method should be called explicitly for a clean shutdown,
        // but the jthread destructor provides a safety net.
        stop();
    }

    Worker* IOPool::get_worker(const size_t id) const
    {
        if (id < workers_.size())
        {
            return workers_[id].get();
        }
        return nullptr;
    }

    size_t IOPool::get_worker_id_by_key(std::string_view key) const { return std::hash<std::string_view>{}(key) % workers_.size(); }

    void IOPool::stop()
    {
        // Request all workers to stop in parallel
        for (const auto& worker: workers_)
        {
            if (worker)
            {
                if (worker->request_stop())
                {
                    spdlog::info("worker {} requested shutdown.", worker->get_id());
                }
                else
                {
                    spdlog::warn("worker {} failed to request shutdown");
                }
            }
        }


        // Wait for all workers to confirm shutdown
        for (const auto& worker: workers_)
        {
            if (worker) worker->wait_shutdown();
        }
        spdlog::info("IOPool has stopped.");

        // The jthread destructors will automatically join, waiting for each thread to finish.
        // We clear the threads here to make that process explicit.
        worker_threads_.clear();
    }
}  // namespace kio::io
