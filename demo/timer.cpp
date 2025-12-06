#include <chrono>

#include "../kio/core/async_logger.h"
#include "../kio/core/sync_wait.h"
#include "../kio/core/worker.h"

using namespace kio;
using namespace io;

Task<std::expected<void, Error>> timer_coroutine(Worker& worker)
{
    // We must switch to the worker thread before using its I/O capabilities.
    co_await SwitchToWorker(worker);

    // 2. Wrap the format string in fmt::runtime()
    ALOG_INFO("Coroutine started. The current time is {:%H:%M:%S}.", std::chrono::system_clock::now());

    ALOG_INFO("Now going to sleep for 5 seconds...");

    // Use KIO_TRY for the void-returning function
    KIO_TRY(co_await worker.async_sleep(std::chrono::seconds(5)));

    // 2. Wrap the format string in fmt::runtime()
    ALOG_INFO("...Woke up! The current time is {:%H:%M:%S}.", std::chrono::system_clock::now());

    ALOG_INFO("Now going to sleep for 500 milliseconds...");
    // 2. Wrap the format string in fmt::runtime()
    KIO_TRY(co_await worker.async_sleep(std::chrono::milliseconds(500)));

    ALOG_INFO("...Woke up again! The current time is {:%H:%M:%S}.", std::chrono::system_clock::now());


    co_return {};
}

int main()
{
    alog::configure(1024, LogLevel::Info);

    constexpr WorkerConfig config{};
    Worker worker(0, config);

    // Start the worker in a background thread.
    std::jthread worker_thread([&](const std::stop_token& st) { worker.loop_forever(); });
    worker.wait_ready();

    ALOG_INFO("--- Running Timer Demo ---");

    if (auto result = SyncWait(timer_coroutine(worker)); !result.has_value())
    {
        ALOG_ERROR("Timer demo failed: {}", result.error());
    }
    else
    {
        ALOG_INFO("Timer demo completed successfully.");
    }

    // Request stop and wait for the worker thread to finish.
    (void) worker.request_stop();

    return 0;
}
