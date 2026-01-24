#include "aio/logger.hpp"
#include <thread>
#include <vector>
#include <cstdio>

int main() {
    // 1. Configuration (Optional)
    // No explicit start() needed! The first log call will trigger it.
    aio::alog::g_level = aio::alog::Level::Debug;

    // 2. Log from Main Thread
    // The background logger thread starts automatically here.
    aio::alog::info("Application initialized. CPU Cores: {}", std::thread::hardware_concurrency());
    aio::alog::debug("Debugging enabled. Variable x = {}", 42);

    // 3. Log from Worker Threads
    std::vector<std::thread> threads;
    for(int i = 0; i < 4; ++i) {
        threads.emplace_back([i] {
            // Threads are automatically registered on their first log
            aio::alog::info("Worker {} started", i);

            for(int j = 0; j < 1000; ++j) {
                if (j % 500 == 0) {
                    aio::alog::warn("Worker {} is halfway done with batch {}", i, j);
                }
            }

            aio::alog::info("Worker {} finished", i);
        });
    }

    for(auto& t : threads) t.join();

    // 4. Cleanup is automatic!
    // When main returns, a static destructor triggers stop() and flushes remaining logs.

    // Optional: Check metrics (Logger is still running here, but idle)
    if (aio::alog::dropped_count() > 0) {
        fprintf(stderr, "Warning: %lu log messages were dropped.\n",
                aio::alog::dropped_count());
    }

    return 0;
}