//
// Created by Yao ACHI on 22/10/2025.
//

#include <chrono>

#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"
#include "spdlog/spdlog.h"

using namespace kio;
using namespace io;

Task<std::expected<void, Error>> timer_coroutine(Worker& worker)
{
    // We must switch to the worker thread before using its I/O capabilities.
    co_await SwitchToWorker(worker);

    spdlog::info("Coroutine started. The current time is {:%H:%M:%S}.", std::chrono::system_clock::now());

    spdlog::info("Now going to sleep for 2 seconds...");

    // Use KIO_TRY to handle potential errors from the sleep operation.
    KIO_TRY(co_await worker.async_sleep(std::chrono::seconds(2)));

    spdlog::info("...Woke up! The current time is {:%H:%M:%S}.", std::chrono::system_clock::now());
    spdlog::info("Now going to sleep for 500 milliseconds...");
    KIO_TRY(co_await worker.async_sleep(std::chrono::milliseconds(500)));
    spdlog::info("...Woke up again! The current time is {:%H:%M:%S}.", std::chrono::system_clock::now());


    co_return {};
}

int main()
{
    spdlog::set_level(spdlog::level::info);

    WorkerConfig config{};
    Worker worker(0, config);

    // Start the worker in a background thread.
    std::jthread worker_thread([&](std::stop_token st) { worker.loop_forever(st); });
    worker.wait_ready();

    spdlog::info("--- Running Timer Demo ---");
    auto result = SyncWait(timer_coroutine(worker));

    if (!result)
    {
        spdlog::error("Timer demo failed: {}", result.error().message());
    }
    else
    {
        spdlog::info("Timer demo completed successfully.");
    }

    // Request stop and wait for the worker thread to finish.
    worker.request_stop();

    return 0;
}
