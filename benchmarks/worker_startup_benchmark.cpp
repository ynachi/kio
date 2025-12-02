//
// Created by Yao ACHI on 19/10/2025.
//

#include <benchmark/benchmark.h>
#include <latch>
#include <thread>

#include "kio/include/async_logger.h"
#include "kio/include/coro.h"
#include "kio/include/io/worker.h"
#include "kio/include/sync_wait.h"

using namespace kio;

// --- Benchmark 1: Callback-based Startup ---
// This pattern is used in tcp_worker_callback.cpp

/**
 * @brief The task to be run by the worker's init callback.
 */
static DetachedTask CallbackTask(std::latch& latch)
{
    // This code runs on the worker thread
    latch.count_down();
    co_return;
}

/**
 * @brief Measures the time to start a worker and have it execute
 * its initial task passed via the init callback.
 */
static void BM_Worker_CallbackStart(benchmark::State& state)
{
    // Suppress logging during benchmark
    alog::configure(1024, LogLevel::Disabled);

    for (auto _ : state)
    {
        std::latch task_ran_latch{1};

        // 1. Define the init callback
        auto init_callback = [&](kio::io::Worker& worker) {
            // Launch the "main" task for this worker
            CallbackTask(task_ran_latch).detach();
        };

        kio::io::WorkerConfig config{};
        config.uring_submit_timeout_ms = 10;
        kio::io::Worker worker(0, config, init_callback);

        // 2. Start the worker thread
        std::jthread worker_thread([&]() {
            worker.loop_forever();
        });

        // 3. Wait for the task to complete.
        // This wait synchronizes, ensuring the benchmark iteration
        // doesn't end until the callback task has actually run.
        task_ran_latch.wait();

        // 4. Stop and cleanup
        worker.request_stop();
    }
}
BENCHMARK(BM_Worker_CallbackStart);

// --- Benchmark 2: SwitchToWorker Startup ---
// This pattern is used in file_demo.cpp

/**
 * @brief The task to be run from an external thread.
 */
static Task<void> SwitchToWorkerTask(kio::io::Worker& worker, std::latch& latch)
{
    // This code starts on the external (main) thread
    co_await kio::io::SwitchToWorker(worker);

    // This code now runs on the worker thread
    latch.count_down();
    co_return;
}

/**
 * @brief Measures the time to start a worker, wait for it to be ready,
 * and then schedule a task onto it from an external thread.
 */
static void BM_Worker_SwitchToWorkerStart(benchmark::State& state)
{
    // Suppress logging during benchmark
    alog::configure(1024, LogLevel::Disabled);

    for (auto _ : state)
    {
        std::latch task_ran_latch{1};

        // 1. Create worker with no callback
        kio::io::WorkerConfig config{};
        config.uring_submit_timeout_ms = 10;
        kio::io::Worker worker(0, config);

        // 2. Start the worker thread
        std::jthread worker_thread([&]() {
            worker.loop_forever();
        });

        // 3. Wait for the worker to be fully initialized
        worker.wait_ready();

        // 4. Run the task from the external thread using SyncWait.
        // SyncWait will block until SwitchToWorkerTask completes.
        kio::SyncWait(SwitchToWorkerTask(worker, task_ran_latch));

        // 5. Stop and cleanup
        worker.request_stop();
        // jthread destructor joins
    }
}
BENCHMARK(BM_Worker_SwitchToWorkerStart);

// --- Main ---
BENCHMARK_MAIN();