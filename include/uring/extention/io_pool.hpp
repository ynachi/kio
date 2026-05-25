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
    std::vector<IO> workers_;
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
        workers_.emplace_back(0, nullptr, opts_);
        const IO* leader = &workers_[0];

        // Followers
        for (std::size_t i = 1; i < num_threads; ++i)
        {
            workers_.emplace_back(i, leader, opts_);
        }

        std::stop_token st = stop_token();

        // now run the threads
        for (std::size_t i = 0; i < num_threads; ++i)
        {
            // run here
            threads_.emplace_back(
                [this, st, i]()
                {
                    IO& io = workers_[i];
                    io.run_blocking(st);
                });
        }
    }

    ~IoContext() = default;

    bool stop() const
    {
        // TODO: we need to do the following
        // io_uring_prep_cancel(..., IORING_ASYNC_CANCEL_ANY);
        // submit_and_wait_for_completions();
        // resume tasks with ECANCELED;
        // destroy remaining scheduled handles;
        if (stop_source_.stop_possible())
        {
            return stop_source_.request_stop();
        }
        return false;
    }

    std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

    [[nodiscard]] IO& worker(const std::size_t idx) { return workers_[idx]; }
    [[nodiscard]] const IO& worker(const std::size_t idx) const { return workers_[idx]; }
    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

    void join() noexcept
    {
        (void)stop();
        workers_.clear();
    }
};
}  // namespace URing