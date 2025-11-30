//
// Created by Yao ACHI on 19/10/2025.
//
#include "kio/include/io/worker_pool.h"

#include "kio/include/async_logger.h"

namespace kio::io
{
    IOPool::IOPool(size_t num_workers, const WorkerConfig &config, const std::function<void(Worker &)> &worker_init)
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
        for (const auto &worker: workers_)
        {
            worker_threads_.emplace_back([&w = *worker] { w.loop_forever(); });
        }

        // Wait for all workers to be fully initialized
        for (const auto &worker: workers_)
        {
            worker->wait_ready();
        }

        ALOG_INFO("IOPool started with {} workers", num_workers);
    }

    IOPool::~IOPool() { stop(); }

    Worker *IOPool::get_worker(const size_t id) const
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
        // stop() is idempotent
        if (stopped_.exchange(true))
        {
            ALOG_DEBUG("IOPool::stop() called but pool is already stopped");
            return;
        }

        // Request all workers to stop in parallel
        for (const auto &worker: workers_)
        {
            if (worker)
            {
                if (worker->request_stop())
                {
                    ALOG_INFO("worker {} requested shutdown.", worker->get_id());
                }
                else
                {
                    ALOG_WARN("worker {} failed to request shutdown", worker->get_id());
                }
            }
        }


        // Wait for all workers to confirm shutdown
        for (const auto &worker: workers_)
        {
            ALOG_DEBUG("WAITING for worker {} to shut down", worker->get_id());
            if (worker) worker->wait_shutdown();
            ALOG_DEBUG("WAITING shutdown has been completed", worker->get_id());
        }
        ALOG_INFO("IOPool has stopped.");

        // TODO: do we need to do that ?
        worker_threads_.clear();
    }
}  // namespace kio::io
