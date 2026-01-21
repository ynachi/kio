#include "aio/logger.hpp"
#include <thread>
#include <vector>
#include <chrono>

int main() {
    // 1. Start the logger (defaults to stderr)
    // The background thread starts here.
    aio::alog::start();

    // 2. Set log level (Optional, defaults to Info)
    aio::alog::g_level = aio::alog::Level::Debug;

    // 3. Log from Main Thread
    aio::alog::info("Application initialized. CPU Cores: {}", std::thread::hardware_concurrency());
    aio::alog::debug("Debugging enabled. Variable x = {}", 42);

    // 4. Log from Worker Threads (Wait-free!)
    std::vector<std::thread> threads;
    for(int i = 0; i < 4; ++i) {
        threads.emplace_back([i] {
            // First log call automatically registers this thread
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

    // 5. Cleanup
    // Stops the thread and flushes remaining messages
    aio::alog::stop();

    // Check if we dropped anything (e.g. if we logged faster than disk could write)
    if (aio::alog::dropped_count() > 0) {
        // fprintf because the logger is stopped
        fprintf(stderr, "Warning: %lu log messages were dropped due to full queues.\n", 
                aio::alog::dropped_count());
    }

    return 0;
}