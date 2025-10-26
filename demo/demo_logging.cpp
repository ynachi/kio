#include "core/include/async_logger.h"
#include <unistd.h>

int main() {
    kio::alog::configure(4096, kio::LogLevel::Disabled);

    ALOG_INFO("Starting app with PID={}", ::getpid());
    ALOG_DEBUG("This is a debug message");
    ALOG_WARN("Low memory warning");
    ALOG_ERROR("Could not open file: {}", "config.json");
    kio::alog::error("Accept failed: {}", 26);
}
