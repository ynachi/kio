// tests/unit/test_task_group.cpp
// Unit tests for task_group RAII container

#include <gtest/gtest.h>
#include "aio/aio.hpp"
#include <chrono>

using namespace aio;
using namespace std::chrono_literals;

// Helper: run context until condition is met
void run_until(io_context& ctx, std::function<bool()> condition,
               std::chrono::milliseconds timeout = 5s) {
    auto start = std::chrono::steady_clock::now();

    while (!condition()) {
        ctx.step();

        if (std::chrono::steady_clock::now() - start > timeout) {
            throw std::runtime_error("Timeout");
        }

        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// ============================================================================
// Basic Functionality Tests
// ============================================================================

class TaskGroupTest : public ::testing::Test {
protected:
    io_context ctx{256};
};

TEST_F(TaskGroupTest, DefaultConstruction) {
    task_group<void> group;

    EXPECT_EQ(group.size(), 0);
    EXPECT_TRUE(group.empty());
    EXPECT_TRUE(group.all_done());
    EXPECT_EQ(group.total_spawned(), 0);
}

TEST_F(TaskGroupTest, SpawnSingleTask) {
    task_group<void> group;

    auto simple_task = [](io_context& ctx) -> task<void> {
        co_await async_sleep(ctx, 10ms);
    };

    group.spawn(simple_task(ctx));

    EXPECT_EQ(group.size(), 1);
    EXPECT_FALSE(group.empty());
    EXPECT_EQ(group.total_spawned(), 1);

    // Task not complete yet
    EXPECT_FALSE(group.all_done());
    EXPECT_EQ(group.active_count(), 1);
}

TEST_F(TaskGroupTest, SpawnMultipleTasks) {
    task_group<void> group;

    for (int i = 0; i < 10; ++i) {
        group.spawn([](io_context& ctx) -> task<void> {
            co_await async_sleep(ctx, 1ms);
        }(ctx));
    }

    EXPECT_EQ(group.size(), 10);
    EXPECT_EQ(group.total_spawned(), 10);
    EXPECT_EQ(group.active_count(), 10);
}

// ============================================================================
// Sweeping Tests
// ============================================================================

TEST_F(TaskGroupTest, ManualSweep) {
    task_group<void> group;

    // Spawn tasks that complete quickly
    for (int i = 0; i < 5; ++i) {
        group.spawn([](io_context& ctx) -> task<void> {
            co_await async_sleep(ctx, 1ms);
        }(ctx));
    }

    // Wait for completion
    run_until(ctx, [&] { return group.all_done(); });

    EXPECT_EQ(group.size(), 5);  // Still in container

    // Sweep removes completed tasks
    size_t removed = group.sweep();

    EXPECT_EQ(removed, 5);
    EXPECT_EQ(group.size(), 0);
    EXPECT_TRUE(group.empty());
}

TEST_F(TaskGroupTest, AutomaticSweeping) {
    task_group<void> group;
    group.set_sweep_interval(4);  // Sweep every 4 spawns

    std::atomic<int> completed{0};

    // Spawn 12 tasks (will trigger 3 auto-sweeps)
    for (int i = 0; i < 12; ++i) {
        group.spawn([&completed](io_context& ctx) -> task<void> {
            co_await async_sleep(ctx, 1ms);
            completed.fetch_add(1, std::memory_order_relaxed);
        }(ctx));

        // Give tasks time to complete before next spawn
        std::this_thread::sleep_for(5ms);
        ctx.step();  // Process completions
    }

    // Wait for all to complete
    run_until(ctx, [&] { return completed.load() == 12; });

    // Final sweep
    group.sweep();

    // Automatic sweeping should have kept size low
    EXPECT_EQ(group.size(), 0);  // All swept
}

TEST_F(TaskGroupTest, SweepIntervalRoundsToPoT) {
    task_group<void> group;

    group.set_sweep_interval(100);  // Not power of 2
    // Should round to 128 (next power of 2)

    // Verify by checking internal behavior
    // (interval is private, so we test indirectly via spawn count)
    for (int i = 0; i < 130; ++i) {
        group.spawn([](io_context& ctx) -> task<void> {
            co_await async_sleep(ctx, 1ms);
        }(ctx));

        std::this_thread::sleep_for(2ms);
        ctx.step();
    }

    // Should have auto-swept at spawn 128
    EXPECT_LT(group.size(), 130);
}

// ============================================================================
// Joining Tests
// ============================================================================

TEST_F(TaskGroupTest, JoinAll) {
    auto test_task = [](io_context& ctx) -> task<void> {
        task_group<void> group;

        for (int i = 0; i < 5; ++i) {
            group.spawn([](io_context& ctx, int id) -> task<void> {
                co_await async_sleep(ctx, 10ms * id);
            }(ctx, i));
        }

        co_await group.join_all(ctx);

        // All tasks should be done
        EXPECT_TRUE(group.all_done());
    };

    auto t = test_task(ctx);
    ctx.run_until_done(t);

    t.result();  // Will throw if any expectations failed
}

TEST_F(TaskGroupTest, JoinAllTimeout) {
    auto test_task = [](io_context& ctx) -> task<void> {
        task_group<void> group;

        // Spawn a long-running task
        group.spawn([](io_context& ctx) -> task<void> {
            co_await async_sleep(ctx, 5s);  // 5 seconds
        }(ctx));

        // Try to join with 100ms timeout
        bool completed = co_await group.join_all_timeout(ctx, 100ms);

        EXPECT_FALSE(completed);  // Should timeout
        EXPECT_FALSE(group.all_done());
    };

    auto t = test_task(ctx);
    ctx.run_until_done(t);

    t.result();
}

// ============================================================================
// Task with Return Values
// ============================================================================

TEST_F(TaskGroupTest, TasksWithReturnValues) {
    auto test_task = [](io_context& ctx) -> task<void> {
        task_group<int> group;

        for (int i = 0; i < 5; ++i) {
            group.spawn([](io_context& ctx, int n) -> task<int> {
                co_await async_sleep(ctx, 1ms);
                co_return n * n;
            }(ctx, i));
        }

        co_await group.join_all(ctx);

        // Extract results
        std::vector<int> results;
        for (auto& task : group.tasks()) {
            if (task.done()) {
                results.push_back(task.result());
            }
        }

        // Should have 5 results: 0, 1, 4, 9, 16
        EXPECT_EQ(results.size(), 5);

        // Sort since order isn't guaranteed
        std::sort(results.begin(), results.end());

        EXPECT_EQ(results[0], 0);
        EXPECT_EQ(results[1], 1);
        EXPECT_EQ(results[2], 4);
        EXPECT_EQ(results[3], 9);
        EXPECT_EQ(results[4], 16);
    };

    auto t = test_task(ctx);
    ctx.run_until_done(t);

    t.result();
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST_F(TaskGroupTest, MoveConstruction) {
    task_group<void> group1;

    group1.spawn([](io_context& ctx) -> task<void> {
        co_await async_sleep(ctx, 1ms);
    }(ctx));

    EXPECT_EQ(group1.size(), 1);

    // Move construct
    task_group<void> group2 = std::move(group1);

    EXPECT_EQ(group2.size(), 1);
    EXPECT_EQ(group2.total_spawned(), 1);
}

TEST_F(TaskGroupTest, MoveAssignment) {
    task_group<void> group1;
    task_group<void> group2;

    group1.spawn([](io_context& ctx) -> task<void> {
        co_await async_sleep(ctx, 1ms);
    }(ctx));

    // Move assign
    group2 = std::move(group1);

    EXPECT_EQ(group2.size(), 1);
    EXPECT_EQ(group2.total_spawned(), 1);
}

// ============================================================================
// Stress Test: Many Tasks
// ============================================================================

TEST_F(TaskGroupTest, ManyTasks) {
    auto test_task = [](io_context& ctx) -> task<void> {
        task_group<void> group;
        group.set_sweep_interval(128);

        std::atomic<int> completed{0};

        // Spawn 500 tasks
        for (int i = 0; i < 500; ++i) {
            group.spawn([&completed](io_context& ctx) -> task<void> {
                co_await async_sleep(ctx, 1ms);
                completed.fetch_add(1, std::memory_order_relaxed);
            }(ctx));
        }

        co_await group.join_all(ctx);

        EXPECT_EQ(completed.load(), 500);
        EXPECT_EQ(group.total_spawned(), 500);

        // Verify all done
        EXPECT_TRUE(group.all_done());
    };

    auto t = test_task(ctx);

    // Run with extended timeout for stress test
    auto start = std::chrono::steady_clock::now();
    while (!t.done()) {
        ctx.step();

        if (std::chrono::steady_clock::now() - start > 30s) {
            FAIL() << "Stress test timeout";
        }

        std::this_thread::sleep_for(100us);
    }

    t.result();
}

// ============================================================================
// Exception Handling
// ============================================================================

TEST_F(TaskGroupTest, TaskExceptionDoesntCrash) {
    auto test_task = [](io_context& ctx) -> task<void> {
        task_group<void> group;

        // Spawn task that throws
        group.spawn([](io_context& ctx) -> task<void> {
            co_await async_sleep(ctx, 1ms);
            throw std::runtime_error("test error");
            co_return;
        }(ctx));

        // Spawn normal task
        group.spawn([](io_context& ctx) -> task<void> {
            co_await async_sleep(ctx, 1ms);
        }(ctx));

        co_await group.join_all(ctx);

        // Both tasks should be done (exception stored in task)
        EXPECT_TRUE(group.all_done());

        // First task should have exception
        EXPECT_THROW(group.tasks()[0].result(), std::runtime_error);

        // Second task should be fine
        EXPECT_NO_THROW(group.tasks()[1].result());
    };

    auto t = test_task(ctx);
    ctx.run_until_done(t);

    t.result();
}

// ============================================================================
// main()
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}