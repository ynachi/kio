//
// Created by Yao ACHI on 19/10/2025.
//

#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <latch>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

#include "core/include/coro.h"
#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"

using namespace kio;
using namespace kio::io;

// --- Test Fixture ---
// Creates a temporary file for the benchmarks to read from.
class CharCountFixture : public benchmark::Fixture {
public:
    const char *test_filename = "char_count_benchmark_file.txt";
    const char char_to_find = 'k';
    size_t expected_count = 0;

    // SetUp is called once per benchmark run
    void SetUp(const ::benchmark::State &state) override {
        // Create a file with a known content and size.
        std::ofstream test_file(test_filename);
        const std::string line = "keep_on_rocking_in_the_free_world_and_making_kio_kick_ass.";

        // Calculate the expected count for one line
        size_t count_per_line = 0;
        for (const char c: line) {
            if (c == char_to_find) {
                count_per_line++;
            }
        }

        // Write enough lines to make the file about 1MB
        constexpr size_t target_size_bytes = 1024 * 1024; // 1 MB
        const size_t num_lines = target_size_bytes / (line.length() + 1);

        for (size_t i = 0; i < num_lines; ++i) {
            test_file << line << '\n';
        }
        test_file.close();

        expected_count = count_per_line * num_lines;
    }

    // TearDown is called once per benchmark run
    void TearDown(const ::benchmark::State &state) override { std::remove(test_filename); }
};

// --- Benchmark 1: SwitchToWorker Pattern ---

/**
 * @brief Coroutine that switches to the worker and then counts characters.
 */
Task<size_t> count_chars_switch_task(Worker &worker, std::string_view filename, char target_char) {
    // 1. Switch from the calling thread to the worker's thread
    co_await SwitchToWorker(worker);

    // 2. Now on the worker thread, perform async operations
    const int fd = co_await worker.async_openat(filename, O_RDONLY, 0);
    if (fd < 0) {
        co_return 0;
    }

    size_t total_count = 0;
    std::vector<char> buffer(8192);
    uint64_t offset = 0;

    while (true) {
        const int bytes_read = co_await worker.async_read(fd, std::span(buffer), offset);
        if (bytes_read <= 0) break; // EOF or error
        total_count += std::count(buffer.data(), buffer.data() + bytes_read, target_char);
        offset += bytes_read;
    }

    co_await worker.async_close(fd);
    co_return total_count;
}

BENCHMARK_F(CharCountFixture, BM_CharCount_SwitchToWorker)(benchmark::State &state) {
    spdlog::set_level(spdlog::level::off);
    for (auto _: state) {
        state.PauseTiming(); // Don't measure setup
        WorkerConfig config{};
        // config.uring_submit_timeout_ms = 0;
        Worker worker(0, config);
        std::jthread worker_thread([&worker] { worker.loop_forever(); });
        worker.wait_ready();
        state.ResumeTiming(); // Start measuring now

        // This is the only part being measured
        const size_t final_count = SyncWait(count_chars_switch_task(worker, test_filename, char_to_find));

        state.PauseTiming(); // Stop measuring for cleanup
        worker.request_stop();
        if (final_count != expected_count) state.SkipWithError("Incorrect count!");
    }
}

// --- Benchmark 2: Callback Pattern ---

/**
 * @brief Detached coroutine that is started by the worker's init callback.
 */
DetachedTask count_chars_callback_task(Worker &worker, std::string_view filename, char target_char,
                                       std::latch &completion_latch, std::atomic<size_t> &result_count) {
    // Already on the worker thread, no need to switch
    const int fd = co_await worker.async_openat(filename, O_RDONLY, 0);
    if (fd < 0) {
        result_count = 0;
        completion_latch.count_down();
        co_return;
    }

    size_t total_count = 0;
    std::vector<char> buffer(8192);
    uint64_t offset = 0;

    while (true) {
        const int bytes_read = (co_await worker.async_read(fd, std::span(buffer), offset)).value();
        if (bytes_read <= 0) break;
        total_count += std::count(buffer.data(), buffer.data() + bytes_read, target_char);
        offset += bytes_read;
    }

    co_await worker.async_close(fd);

    result_count = total_count;
    completion_latch.count_down(); // Signal completion
}

BENCHMARK_F(CharCountFixture, BM_CharCount_Callback)(benchmark::State &state) {
    spdlog::set_level(spdlog::level::off);
    for (auto _: state) {
        state.PauseTiming(); // Don't measure setup
        std::latch completion_latch{1};
        std::atomic<size_t> final_count{0};
        auto init_callback = [&](Worker &worker) {
            count_chars_callback_task(worker, test_filename, char_to_find, completion_latch, final_count).detach();
        };
        WorkerConfig config{};
        // config.uring_submit_timeout_ms = 0;
        Worker worker(0, config, init_callback);
        std::jthread worker_thread([&worker] { worker.loop_forever(); });
        worker.wait_ready(); // Wait for the worker to be ready before we start the clock
        state.ResumeTiming(); // Start measuring now

        // The only thing we measure is waiting for the pre-scheduled task to finish.
        completion_latch.wait();

        state.PauseTiming(); // Stop measuring for cleanup
        worker.request_stop();
        if (final_count.load() != expected_count) state.SkipWithError("Incorrect count!");
    }
}
