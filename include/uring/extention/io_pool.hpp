#pragma once
#include "uring/core/io.h"

namespace URing
{

/// IoContext serves as an example of IO pool.
/// Advanced use-cases should prefer working with the IO class
/// directly.
class IoContext
{
    std::stop_source stop_source_;
    IoOptions opts_;
    std::vector<std::unique_ptr<IO>> workers_;
    std::vector<std::jthread> threads_;

public:
    explicit IoContext(const std::size_t num_threads, const IoOptions& opts = {}) : opts_(opts)
    {
        if (num_threads == 0)
        {
            throw std::runtime_error("io context started with 0 thread");
        }

        workers_.reserve(num_threads);
        threads_.reserve(num_threads);

        // Leader (Worker 0)
        workers_.push_back(std::make_unique<IO>(0, nullptr, opts_));
        const IO* leader = workers_[0].get();

        // Followers
        for (std::size_t i = 1; i < num_threads; ++i)
        {
            workers_.push_back(std::make_unique<IO>(i, leader, opts_));
        }

        std::stop_token st = stop_token();

        // If thread creation fails, stop existing workers before jthread joins.
        try
        {
            for (std::size_t i = 0; i < num_threads; ++i)
                threads_.emplace_back([this, st, i]() { workers_[i]->run(st); });
        }
        catch (...)
        {
            (void)stop();
            throw;
        }
    }

    ~IoContext() { join(); }

    bool stop() const
    {
        if (stop_source_.stop_possible())
        {
            return stop_source_.request_stop();
        }
        return false;
    }

    std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

    [[nodiscard]] IO& worker(const std::size_t idx) { return *workers_[idx]; }
    [[nodiscard]] const IO& worker(const std::size_t idx) const { return *workers_[idx]; }
    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

    void join() noexcept
    {
        (void)stop();

        for (auto& thread : threads_)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }

        threads_.clear();
        workers_.clear();
    }
};
}  // namespace URing
