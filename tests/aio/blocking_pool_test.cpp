// tests/stress/test_blocking_pool_tsan.cpp
// Stress test to catch race conditions in blocking_pool
// Run with: g++ -fsanitize=thread -g -O1 test_blocking_pool_tsan.cpp -lgtest -luring

#include <gtest/gtest.h>
#include "aio/aio.hpp"
#include <atomic>
#include <vector>

using namespace aio;
using namespace std::chrono_literals;

// ============================================================================
// Race Condition Detection Tests (requires TSAN)
// ============================================================================

class BlockingPoolTSANTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Verify we're running with TSAN
        #ifndef __SANITIZE_THREAD__
        GTEST_SKIP() << "This test requires ThreadSanitizer (-fsanitize=thread)";
        #endif
    }

    io_context ctx{256};
    blocking_pool pool{8};  // Many threads to increase race probability
};

// This test would have caught the use-after-free bug in offload_op!
TEST_F(BlockingPoolTSANTest, ConcurrentOffloadOperations) {
    std::atomic<int> completed{0};

    auto test_task = [&](io_context& ctx, blocking_pool& pool) -> task<void> {
        // Spawn many offload operations concurrently
        std::vector<task<void>> tasks;

        for (int i = 0; i < 100; ++i) {
            tasks.push_back([&, i](io_context& ctx, blocking_pool& pool) -> task<void> {
                // Offload work that completes quickly
                auto result = co_await offload(ctx, pool, [i]() {
                    // Minimal work to maximize race window
                    return i;
                });

                EXPECT_EQ(result, i);
                completed.fetch_add(1, std::memory_order_relaxed);
            }(ctx, pool));
        }

        // Start all tasks
        for (auto& t : tasks) {
            t.start();
        }

        // Wait for all to complete
        while (completed.load(std::memory_order_relaxed) < 100) {
            ctx.step();
            std::this_thread::sleep_for(1ms);
        }

        co_return;
    };

    auto t = test_task(ctx, pool);
    ctx.run_until_done(t);

    t.result();

    EXPECT_EQ(completed.load(), 100);
}

// Test rapid offload/completion cycle
TEST_F(BlockingPoolTSANTest, RapidOffloadCompletion) {
    std::atomic<int> count{0};

    auto test_task = [&](io_context& ctx, blocking_pool& pool) -> task<void> {
        // Repeatedly offload very fast operations
        for (int i = 0; i < 1000; ++i) {
            auto result = co_await offload(ctx, pool, [&count]() {
                count.fetch_add(1, std::memory_order_relaxed);
                return 42;
            });

            EXPECT_EQ(result, 42);

            // Occasionally yield to let other work happen
            if (i % 10 == 0) {
                co_await async_sleep(ctx, 1ms);
            }
        }
    };

    auto t = test_task(ctx, pool);
    ctx.run_until_done(t);

    t.result();

    EXPECT_EQ(count.load(), 1000);
}

// Test offload with move-only types (tests the fixed capture bug)
TEST_F(BlockingPoolTSANTest, OffloadWithMoveOnlyCapture) {
    auto test_task = [](io_context& ctx, blocking_pool& pool) -> task<void> {
        std::vector<task<size_t>> tasks;

        for (int i = 0; i < 50; ++i) {
            tasks.push_back([i](io_context& ctx, blocking_pool& pool) -> task<size_t> {
                auto data = std::make_unique<std::vector<int>>(1000, i);

                // This tests that moving data into the lambda works correctly
                auto result = co_await offload(ctx, pool,
                    [data = std::move(data)]() {
                        return data->size();
                    });

                co_return result;
            }(ctx, pool));
        }

        // Start all
        for (auto& t : tasks) {
            t.start();
        }

        // Wait for all
        bool all_done = false;
        while (!all_done) {
            ctx.step();
            all_done = std::all_of(tasks.begin(), tasks.end(),
                [](const task<size_t>& t) { return t.done(); });
            std::this_thread::sleep_for(1ms);
        }

        // Verify results
        for (auto& t : tasks) {
            EXPECT_EQ(t.result(), 1000);
        }
    };

    auto t = test_task(ctx, pool);
    ctx.run_until_done(t);

    t.result();
}

// Test exception propagation under load
TEST_F(BlockingPoolTSANTest, ExceptionPropagationUnderLoad) {
    std::atomic<int> exceptions_caught{0};

    auto test_task = [&](io_context& ctx, blocking_pool& pool) -> task<void> {
        std::vector<task<void>> tasks;

        for (int i = 0; i < 100; ++i) {
            tasks.push_back([&, i](io_context& ctx, blocking_pool& pool) -> task<void> {
                try {
                    co_await offload(ctx, pool, [i]() {
                        if (i % 3 == 0) {
                            throw std::runtime_error("test error");
                        }
                    });
                } catch (const std::runtime_error&) {
                    exceptions_caught.fetch_add(1, std::memory_order_relaxed);
                }
            }(ctx, pool));
        }

        // Start all
        for (auto& t : tasks) {
            t.start();
        }

        // Wait
        bool all_done = false;
        while (!all_done) {
            ctx.step();
            all_done = std::all_of(tasks.begin(), tasks.end(),
                [](const task<void>& t) { return t.done(); });
            std::this_thread::sleep_for(1ms);
        }
    };

    auto t = test_task(ctx, pool);
    ctx.run_until_done(t);

    t.result();

    // Should have caught 34 exceptions (100 / 3, rounded up)
    EXPECT_GE(exceptions_caught.load(), 33);
    EXPECT_LE(exceptions_caught.load(), 34);
}

// ============================================================================
// Memory Access Pattern Test
// ============================================================================

// This test specifically targets the use-after-free pattern
TEST_F(BlockingPoolTSANTest, MemoryAccessPattern) {
    auto test_task = [](io_context& ctx, blocking_pool& pool) -> task<void> {
        // Create many operations that complete at different times
        std::vector<task<int>> tasks;

        for (int i = 0; i < 200; ++i) {
            tasks.push_back([i](io_context& ctx, blocking_pool& pool) -> task<int> {
                // Variable sleep to create different completion patterns
                std::this_thread::sleep_for(std::chrono::microseconds(i % 100));

                auto result = co_await offload(ctx, pool, [i]() {
                    // Some work
                    int sum = 0;
                    for (int j = 0; j < 100; ++j) {
                        sum += j;
                    }
                    return sum + i;
                });

                co_return result;
            }(ctx, pool));
        }

        // Start with some delay to stagger starts
        for (size_t i = 0; i < tasks.size(); ++i) {
            tasks[i].start();
            if (i % 20 == 0) {
                co_await async_sleep(ctx, 5ms);
            }
        }

        // Wait for completion
        bool all_done = false;
        while (!all_done) {
            ctx.step();
            all_done = std::all_of(tasks.begin(), tasks.end(),
                [](const task<int>& t) { return t.done(); });
            std::this_thread::sleep_for(500us);
        }

        // Verify all results
        for (size_t i = 0; i < tasks.size(); ++i) {
            int expected = (99 * 100) / 2 + static_cast<int>(i);  // sum(0..99) + i
            EXPECT_EQ(tasks[i].result(), expected);
        }
    };

    auto t = test_task(ctx, pool);

    // Run with timeout
    auto start = std::chrono::steady_clock::now();
    while (!t.done()) {
        ctx.step();

        if (std::chrono::steady_clock::now() - start > 30s) {
            FAIL() << "Test timeout";
        }

        std::this_thread::sleep_for(100us);
    }

    t.result();
}

// ============================================================================
// How to Run These Tests
// ============================================================================

/*
Build and run:

# TSAN build
g++ -std=c++23 -fsanitize=thread -g -O1 \
    test_blocking_pool_tsan.cpp \
    -I../include \
    -lgtest -lgtest_main -luring -lpthread \
    -o test_blocking_pool_tsan

# Run
./test_blocking_pool_tsan

# Expected output (with buggy code):
# ==================
# WARNING: ThreadSanitizer: data race (pid=...)
#   Write of size 8 at 0x... by thread T9
#   Previous read of size 8 at 0x... by thread T1
# ==================

# Expected output (with fixed code):
# [==========] Running X tests from 1 test suite.
# [       OK ] BlockingPoolTSANTest.ConcurrentOffloadOperations
# [       OK ] BlockingPoolTSANTest.RapidOffloadCompletion
# ...
# [==========] X tests from 1 test suite ran. (Xms total)
# [  PASSED  ] X tests.
# ThreadSanitizer: no issues found

# CMake integration:
add_executable(blocking_pool_tsan_tests
    tests/stress/test_blocking_pool_tsan.cpp
)
target_compile_options(blocking_pool_tsan_tests PRIVATE -fsanitize=thread)
target_link_options(blocking_pool_tsan_tests PRIVATE -fsanitize=thread)
target_link_libraries(blocking_pool_tsan_tests aio GTest::gtest GTest::gtest_main)

# Add to CTest but mark as requiring TSAN
add_test(NAME blocking_pool_tsan COMMAND blocking_pool_tsan_tests)
set_tests_properties(blocking_pool_tsan PROPERTIES
    LABELS "tsan;stress"
    TIMEOUT 60
)
*/

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}