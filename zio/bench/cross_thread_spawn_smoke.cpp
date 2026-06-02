#include "../io_context.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <system_error>
#include <thread>
#include <vector>

int main()
{
    constexpr std::size_t producers = 4;
    constexpr std::size_t jobs_per_producer = 128;
    constexpr std::size_t total_jobs = producers * jobs_per_producer;

    zio::io_context ctx{zio::io_context_options{
        .ring_entries = 1024,
        .ring_flags = 0,
        .fiber_count = 1024,
        .stack_size = 64 * 1024,
        .ready_budget = 128,
    }};

    std::atomic_size_t ran{0};
    std::atomic_size_t schedule_errors{0};
    std::error_code run_error;

    std::jthread runner(
        [&](std::stop_token st)
        {
            auto res = ctx.run_blocking(st);
            if (!res)
                run_error = res.error();
        });

    std::vector<std::jthread> threads;
    threads.reserve(producers);

    for (std::size_t p = 0; p < producers; ++p)
    {
        threads.emplace_back(
            [&]
            {
                for (std::size_t i = 0; i < jobs_per_producer; ++i)
                {
                    auto res = ctx.schedule(
                        [&]
                        {
                            if (ran.fetch_add(1, std::memory_order_acq_rel) + 1 == total_jobs)
                                ctx.request_stop();
                        });
                    if (!res)
                        schedule_errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    threads.clear();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ran.load(std::memory_order_acquire) != total_jobs &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ctx.request_stop();
    runner.request_stop();
    runner.join();

    if (schedule_errors.load(std::memory_order_relaxed) != 0)
    {
        std::cerr << "schedule errors=" << schedule_errors.load(std::memory_order_relaxed) << '\n';
        return 1;
    }
    if (run_error)
    {
        std::cerr << "run error: " << run_error.message() << '\n';
        return 1;
    }
    if (ran.load(std::memory_order_acquire) != total_jobs)
    {
        std::cerr << "ran=" << ran.load(std::memory_order_acquire) << " expected=" << total_jobs << '\n';
        return 1;
    }

    std::cout << "cross-thread spawn smoke passed jobs=" << total_jobs << '\n';
    return 0;
}
