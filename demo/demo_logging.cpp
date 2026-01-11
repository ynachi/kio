#include "../kio/core/async_logger.h"

#include <unistd.h>

int main()
{
    kio::alog::Configure(4096, kio::LogLevel::kInfo);

    ALOG_INFO("Starting app with PID={}", ::getpid());
    ALOG_DEBUG("This is a debug message");
    ALOG_WARN("Low memory warning");
    ALOG_ERROR("Could not open file: {}", "config.json");
}
