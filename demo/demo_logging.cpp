//
// Created by Yao ACHI on 25/10/2025.
//

#include "core/include/async_logger.h"
#include <thread>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

int main() {
    kio::Logger logger(1024, kio::LogLevel::Trace);

    KIO_LOG_INFO(logger, "Starting app with PID={}", ::getpid());
    LOGD(logger, "Debug info: {}", 42);
    LOGW(logger, "Something might be wrong...");
    KIO_LOG_ERROR(logger, "Oh lets test error");
}
