#include <chrono>

#include "core/include/io/worker.h"
#include "core/include/net.h"
#include "core/include/sync_wait.h"
#include "core/include/errors.h"
#include "core/include/async_logger.h"

using namespace kio;
using namespace io;
using namespace net;

/**
 * @brief Attempts to connect to a server, retrying with exponential backoff.
 */
Task<Result<int>> connect_with_retries(Worker& worker)
{
    co_await SwitchToWorker(worker);

    // This is an address we know will refuse connection.
    // Use the 2-argument KIO_TRY for assignment
    SocketAddress addr = KIO_TRY(parse_address("127.0.0.1", 8080));

    int max_retries = 5;
    auto delay = std::chrono::milliseconds(200);

    for (int i = 0; i < max_retries; ++i)
    {
        // Use the 2-argument KIO_TRY for assignment
        int fd = KIO_TRY(create_raw_socket(addr.family));

        ALOG_INFO("Attempt {}/{} to connect...", i + 1, max_retries);
        // Add reinterpret_cast to match const sockaddr*
        auto connect_result = co_await worker.async_connect(fd, reinterpret_cast<const sockaddr*>(&addr.addr), addr.addrlen);

        if (connect_result)
        {
            ALOG_INFO("Connection successful on attempt {}!", i + 1);
            co_return fd;  // Success!
        }

        // Connection failed. Log it, close the failed fd, and wait.
        ALOG_WARN("Connect failed: {}. Retrying in {:.1f}s.", connect_result.error().message(), std::chrono::duration<double>(delay).count());

        close(fd);

        // Use the 1-argument KIO_TRY for a void expression
        KIO_TRY(co_await worker.async_sleep(delay));

        // Double the delay for the next attempt (exponential backoff)
        delay *= 2;
    }

    ALOG_ERROR("All {} connection attempts failed.", max_retries);
    co_return std::unexpected(Error::from_errno(ETIMEDOUT));
}

int main()
{
    alog::configure(4096, LogLevel::Info);
    Worker worker(0, {});
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    if (auto result = SyncWait(connect_with_retries(worker)); !result)
    {
        ALOG_ERROR("connect_with_retries failed: {}", result.error().message());
    }

    // Cast to (void) to suppress the nodiscard warning
    (void) worker.request_stop();
    return 0;
}
