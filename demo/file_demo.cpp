//
// Created by Yao ACHI on 19/10/2025.
//
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <latch>
#include <numeric>
#include <thread>

#include "core/include/coro.h"
#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"
#include "spdlog/spdlog.h"

using namespace kio;
using namespace kio::io;

/**
 * @brief A coroutine that reads a file and counts occurrences of a character.
 *
 * This demonstrates starting on one thread, switching to the worker, and
 * then performing all I/O operations on the worker's thread.
 *
 * @param worker The Worker context to perform operations on.
 * @param filename The path to the file.
 * @param target_char The character to count.
 * @return A Task containing the final count.
 */
Task<size_t> count_chars_in_file(Worker& worker, std::string_view filename, char target_char)
{
    spdlog::info("[main coro] Starting on thread ID: {}", std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // Switch execution from the calling thread (main) to the worker's thread.
    co_await SwitchToWorker(worker);

    spdlog::info("[main coro] Switched! Now running on worker thread ID: {}", std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // Now that we are on the correct thread, we can safely call async methods.
    int fd = co_await worker.async_openat(filename, O_RDONLY, 0);
    if (fd < 0)
    {
        spdlog::error("Failed to open file '{}': {}", filename, strerror(-fd));
        co_return 0;
    }
    spdlog::info("Successfully opened file '{}', fd={}", filename, fd);

    size_t total_count = 0;
    constexpr size_t buffer_size = 8192;
    std::vector<char> buffer(buffer_size);
    uint64_t offset = 0;

    while (true)
    {
        int bytes_read = co_await worker.async_read(fd, std::span(buffer.data(), buffer.size()), offset);

        if (bytes_read == 0)
        {
            spdlog::info("Reached end of file.");
            break;  // EOF
        }
        if (bytes_read < 0)
        {
            spdlog::error("Error reading file: {}", strerror(-bytes_read));
            break;
        }

        total_count += std::count(buffer.data(), buffer.data() + bytes_read, target_char);
        offset += bytes_read;
    }

    co_await worker.async_close(fd);
    spdlog::info("Closed file descriptor {}", fd);

    co_return total_count;
}

int main()
{
    spdlog::set_level(spdlog::level::info);

    // Create a dummy file for the demo
    const char* test_filename = "test_file.txt";
    std::ofstream test_file(test_filename);
    test_file << "Hello world, this is a test file for our coroutine worker.\n";
    test_file << "Let's test switching threads and reading files asynchronously.\n";
    test_file.close();
    char char_to_find = 'e';

    // --- New Worker and Thread Setup ---
    WorkerConfig config{};
    // Create the worker context. It doesn't start any threads itself.
    Worker worker(0, config);

    // Manually create a thread and assign the worker's event loop to it.
    std::jthread worker_thread(
            [&worker]
            {
                spdlog::info("Worker thread starting with ID: {}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
                //  This thread will now block here, running the I/O event loop.
                worker.loop_forever();
                spdlog::info("Worker thread finished its loop.");
            });

    // Wait for the worker to complete its initialization on the new thread.
    worker.wait_ready();
    spdlog::info("Worker context has been initialized on its thread.");

    // --- Run the Coroutine on the Main Thread ---
    // The coroutine will start here, then hop to the worker_thread via SwitchToWorker.
    size_t final_count = SyncWait(count_chars_in_file(worker, test_filename, char_to_find));

    // --- Cleanup ---
    spdlog::info("Main thread requesting worker to stop.");
    worker.request_stop();  // Signals the stop_source and wakes up the loop.

    // The jthread's destructor will automatically call join(), ensuring
    // we wait for the worker thread to finish cleanly.

    std::cout << "\n--- Demo Complete ---\n";
    std::cout << "Found the character '" << char_to_find << "' " << final_count << " times in " << test_filename << ".\n";

    // Clean up the dummy file
    std::remove(test_filename);

    return 0;
}
