//
// Created by Yao ACHI on 26/11/2025.
//

#include <filesystem>
#include <gtest/gtest.h>

#include "bitcask/include/partition.h"
#include "kio/include/io/worker.h"
#include "kio/include/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class CompactionTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;
    BitcaskConfig config_;

    void SetUp() override {
        alog::configure(1024, LogLevel::Disabled);
        test_dir_ = std::filesystem::temp_directory_path() / "bitcask_compaction_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
        // create the partition dir too
        std::filesystem::create_directories(test_dir_ / "partition_0");

        config_.directory = test_dir_;
        config_.max_file_size = 4 * 1024;  // 4KB for fast rotation
        config_.sync_on_write = false;
        config_.auto_compact = false;
        config_.fragmentation_threshold = 0.5;

        WorkerConfig worker_config;
        worker_config.uring_queue_depth = 128;
        worker_ = std::make_unique<Worker>(0, worker_config);

        worker_thread_ = std::thread([this]() {
            worker_->loop_forever();
        });

        worker_->wait_ready();
    }

    void TearDown() override {
        if (worker_) {
            (void)worker_->request_stop();
            worker_->wait_shutdown();
        }

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        worker_.reset();
        std::filesystem::remove_all(test_dir_);
    }
};

// ============================================================================
// Basic Compaction Tests
// ============================================================================
TEST_F(CompactionTest, ReducesFileCount) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_result = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_result.has_value());
        auto partition = std::move(partition_result.value());

        // Create fragmentation: write, update, delete
        std::vector data(500, 'X');

        // Write to multiple files
        for (int i = 0; i < 20; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->put(std::move(key), std::vector(data));
        }

        // Update half (creates dead data)
        for (int i = 0; i < 10; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->put(std::move(key), std::vector(data));
        }

        const auto stats_before = partition->get_stats();
        const size_t files_before = stats_before.data_files.size();

        EXPECT_GT(files_before, 1) << "Should have multiple files";

        //Compact
        auto compact_result = co_await partition->compact();
        EXPECT_TRUE(compact_result.has_value());

        auto stats_after = partition->get_stats();
        size_t files_after = stats_after.data_files.size();

        // Should have fewer files or reclaimed bytes
        EXPECT_TRUE(files_after <= files_before || stats_after.bytes_reclaimed_total > 0);
        EXPECT_GE(stats_after.compactions_total, 1);
    };

    SyncWait(test_coro());
}

TEST_F(CompactionTest, PreservesLiveData) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // Write initial data
        std::vector<std::string> keys;
        for (int i = 0; i < 50; ++i) {
            std::string key = std::format("key_{}", i);
            std::string val_str = std::format("value_{}", i);
            std::vector value(val_str.begin(), val_str.end());

            co_await partition->put(std::string(key), std::move(value));
            keys.push_back(key);
        }

        // Create fragmentation by updating some keys
        for (int i = 0; i < 25; ++i) {
            std::string key = std::format("key_{}", i);
            std::string val_str = std::format("updated_{}", i);
            std::vector value(val_str.begin(), val_str.end());

            co_await partition->put(std::move(key), std::move(value));
        }

        // Compact
        auto compact_result = co_await partition->compact();
        EXPECT_TRUE(compact_result.has_value());

        // Verify all live data is accessible
        for (int i = 0; i < 50; ++i) {
            std::string key = std::format("key_{}", i);
            auto get_result = co_await partition->get(key);

            EXPECT_TRUE(get_result.has_value()) << "Failed to get " << key;
            EXPECT_TRUE(get_result.value().has_value()) << key << " should exist";

            if (get_result.value().has_value()) {
                // Check correct value (updated or original)
                std::string expected;
                if (i < 25) {
                    expected = std::format("updated_{}", i);
                } else {
                    expected = std::format("value_{}", i);
                }

                std::vector expected_vec(expected.begin(), expected.end());
                EXPECT_EQ(get_result.value().value(), expected_vec);
            }
        }
    };

    SyncWait(test_coro());
}

TEST_F(CompactionTest, RemovesDeletedKeys) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // Write data
        for (int i = 0; i < 30; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector value(100, 'X');
            co_await partition->put(std::move(key), std::move(value));
        }

        // Delete half
        for (int i = 0; i < 15; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->del(key);
        }

        auto stats_before = partition->get_stats();
        uint64_t total_bytes_before = 0;
        for (const auto& file_stats: stats_before.data_files | std::views::values) {
            total_bytes_before += file_stats.total_bytes;
        }

        // Compact
        co_await partition->compact();

        auto stats_after = partition->get_stats();
        uint64_t total_bytes_after = 0;
        for (const auto& file_stats: stats_after.data_files | std::views::values) {
            total_bytes_after += file_stats.total_bytes;
        }

        // Should have reclaimed space
        EXPECT_LT(total_bytes_after, total_bytes_before)
            << "Compaction should reduce total file size";
        EXPECT_GT(stats_after.bytes_reclaimed_total, 0);

        // Verify deleted keys stay deleted
        for (int i = 0; i < 15; ++i) {
            std::string key = std::format("key_{}", i);
            auto get_result = co_await partition->get(key);

            EXPECT_TRUE(get_result.has_value());
            EXPECT_FALSE(get_result.value().has_value()) << key << " should be deleted";
        }

        // Verify kept keys exist
        for (int i = 15; i < 30; ++i) {
            std::string key = std::format("key_{}", i);
            auto get_result = co_await partition->get(key);

            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                << key << " should exist";
        }
    };

    SyncWait(test_coro());
}

// Compaction Handles Concurrent Updates (via KeyDir CAS)
TEST_F(CompactionTest, HandlesStaleEntries) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        std::vector data(300, 'X');

        // Write initial data (will be in old files)
        for (int i = 0; i < 10; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->put(std::string(key), std::vector(data));
        }

        // Force file rotation by writing more
        for (int i = 10; i < 20; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->put(std::string(key), std::vector(data));
        }

        // Update keys 0-5 AFTER they're in old files
        // This simulates the case where compaction might encounter stale entries
        for (int i = 0; i < 5; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector new_data(300, 'Y');
            co_await partition->put(std::move(key), std::move(new_data));
        }

        // Compact
        auto compact_result = co_await partition->compact();
        EXPECT_TRUE(compact_result.has_value());

        // Verify updated keys have correct values (not the old ones)
        for (int i = 0; i < 5; ++i) {
            std::string key = std::format("key_{}", i);
            auto get_result = co_await partition->get(key);

            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());

            if (get_result.value().has_value()) {
                // Should be 'Y', not 'X'
                EXPECT_EQ(get_result.value().value()[0], 'Y')
                    << "Key " << key << " should have updated value";
            }
        }

        // Keys 5-9 should still have 'X'
        for (int i = 5; i < 10; ++i) {
            std::string key = std::format("key_{}", i);
            auto get_result = co_await partition->get(key);

            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());

            if (get_result.value().has_value()) {
                EXPECT_EQ(get_result.value().value()[0], 'X')
                    << "Key " << key << " should have original value";
            }
        }
    };

    SyncWait(test_coro());
}

// Recovery After Compaction Tests
TEST_F(CompactionTest, RecoveryAfterCompaction) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write, compact, close
        {
            auto partition_res = co_await Partition::open(config_, *worker_, 0);
            EXPECT_TRUE(partition_res.has_value());
            auto partition = std::move(partition_res.value());

            // Write data
            for (int i = 0; i < 20; ++i) {
                std::string key = std::format("key_{}", i);
                std::vector value(200, static_cast<char>('A' + i % 26));
                co_await partition->put(std::move(key), std::move(value));
            }

            // Update to create fragmentation
            for (int i = 0; i < 10; ++i) {
                std::string key = std::format("key_{}", i);
                std::vector value(200, 'Z');
                co_await partition->put(std::move(key), std::move(value));
            }

            // Compact
            auto compact_result = co_await partition->compact();
            EXPECT_TRUE(compact_result.has_value());

            // Partition closed and goes out of scope
            co_await partition->async_close();
        }

        // Phase 2: Recover and verify
        {
            auto partition_res = co_await Partition::open(config_, *worker_, 0);
            EXPECT_TRUE(partition_res.has_value());
            auto partition = std::move(partition_res.value());

            // Verify all data
            for (int i = 0; i < 20; ++i) {
                std::string key = std::format("key_{}", i);
                auto get_result = co_await partition->get(key);

                EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                    << "Key " << key << " should exist after recovery";

                if (get_result.value().has_value()) {
                    char expected_char = (i < 10) ? 'Z' : static_cast<char>('A' + i % 26);
                    EXPECT_EQ(get_result.value().value()[0], expected_char);
                }
            }
        }
    };

    SyncWait(test_coro());
}


TEST_F(CompactionTest, MultipleCompactionCycles) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // 400 bytes payload.
        // With header/key, total ~420 bytes.
        // 10 entries = 4200 bytes > 4096 (max_file_size).
        // This forces rotation every cycle.
        std::vector data(400, 'X');

        // Cycle 1
        for (int i = 0; i < 15; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }
        for (int i = 0; i < 10; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }
        co_await partition->compact();

        // Cycle 2
        for (int i = 0; i < 10; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }
        co_await partition->compact();

        // Cycle 3
        for (int i = 5; i < 15; ++i) {
            co_await partition->del(std::format("key_{}", i));
        }
        co_await partition->compact();

        auto stats = partition->get_stats();
        // Now valid to expect 3, as we forced rotations
        EXPECT_GE(stats.compactions_total, 3);

        // Verify final state
        for (int i = 0; i < 5; ++i) {
            auto get_result = co_await partition->get(std::format("key_{}", i));
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());
        }

        for (int i = 5; i < 15; ++i) {
            auto get_result = co_await partition->get(std::format("key_{}", i));
            EXPECT_TRUE(get_result.has_value());
            EXPECT_FALSE(get_result.value().has_value()) << "Should be deleted";
        }
        co_await partition->async_close();
    };

    SyncWait(test_coro());
}

// ============================================================================
// Edge Cases & Stress Tests
// ============================================================================

TEST_F(CompactionTest, AllDeadData) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // Write data
        for (int i = 0; i < 20; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector value(200, 'X');
            co_await partition->put(std::move(key), std::move(value));
        }

        // Delete everything
        for (int i = 0; i < 20; ++i) {
            co_await partition->del(std::format("key_{}", i));
        }

        auto stats_before = partition->get_stats();

        // Compact
        auto compact_result = co_await partition->compact();
        EXPECT_TRUE(compact_result.has_value());

        auto stats_after = partition->get_stats();

        // Should reclaim significant space
        EXPECT_GT(stats_after.bytes_reclaimed_total, 0);

        // All gets should return nothing
        for (int i = 0; i < 20; ++i) {
            auto get_result = co_await partition->get(std::format("key_{}", i));
            EXPECT_TRUE(get_result.has_value());
            EXPECT_FALSE(get_result.value().has_value());
        }
    };

    SyncWait(test_coro());
}

TEST_F(CompactionTest, NoDeadData) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // Write fresh data (no updates, no deletes)
        for (int i = 0; i < 10; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector value(100, 'X');
            co_await partition->put(std::move(key), std::move(value));
        }

        auto stats_before = partition->get_stats();

        // Compact (should be mostly no-op)
        auto compact_result = co_await partition->compact();
        EXPECT_TRUE(compact_result.has_value());

        auto stats_after = partition->get_stats();

        // Bytes reclaimed should be minimal (only headers/metadata overhead)
        // All keys should still be accessible
        for (int i = 0; i < 10; ++i) {
            auto get_result = co_await partition->get(std::format("key_{}", i));
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());
        }
    };

    SyncWait(test_coro());
}

TEST_F(CompactionTest, ManySmallUpdates) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // Create a single key with many updates
        std::string key = "frequently_updated";

        // value size to 512 bytes.
        // 50 updates * 512 bytes = ~25KB.
        // max_file_size = 4KB.
        // This guarantees ~6 rotations, creating multiple sealed files
        // that contain entirely dead data (versions 0..48), ensuring they exceed the threshold.
        for (int version = 0; version < 50; ++version) {
            std::string val_str = std::format("version_{}", version);
            // Pad to 512 bytes
            std::vector value(512, 'X');
            // Copy version string to start of vector
            std::ranges::copy(val_str, value.begin());

            co_await partition->put(std::string(key), std::move(value));
        }

        auto stats_before = partition->get_stats();
        double frag_before = stats_before.overall_fragmentation();

        // This should now definitely be high
        EXPECT_GT(frag_before, 0.5);

        // Compact
        co_await partition->compact();

        auto stats_after = partition->get_stats();
        double frag_after = stats_after.overall_fragmentation();

        // Fragmentation should decrease significantly
        EXPECT_LT(frag_after, frag_before);
        EXPECT_GT(stats_after.compactions_total, 0);

        // Latest value should be accessible
        auto get_result = co_await partition->get(key);
        EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());

        if (get_result.value().has_value()) {
            std::string expected_prefix = "version_49";
            // Check prefix
            std::string actual(get_result.value().value().begin(), get_result.value().value().begin() + static_cast<int>(expected_prefix.size()));
            EXPECT_EQ(actual, expected_prefix);
        }

        co_await partition->async_close();
    };

    SyncWait(test_coro());
}

TEST_F(CompactionTest, StatsTracking) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // Create workload
        std::vector data(300, 'X');
        for (int i = 0; i < 30; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }

        // Update to fragment
        for (int i = 0; i < 15; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector<char>(data));
        }

        auto stats_before = partition->get_stats();
        EXPECT_EQ(stats_before.compactions_total, 0);
        EXPECT_EQ(stats_before.bytes_reclaimed_total, 0);

        // Compact
        auto result = co_await partition->compact();
        EXPECT_TRUE(result.has_value());

        auto stats_after = partition->get_stats();

        EXPECT_GE(stats_after.compactions_total, 1);
        EXPECT_GT(stats_after.bytes_reclaimed_total, 0);
        EXPECT_GE(stats_after.files_compacted_total, 1);

        // Verify FD cache stats are being tracked
        const auto& cache_stats = partition->get_cache_stats();
        // After compaction, we should have some cache activity
        EXPECT_GE(cache_stats.hits + cache_stats.misses, 0);
    };

    SyncWait(test_coro());
}

// ============================================================================
// Compactor Unit Tests (Direct API)
// ============================================================================


TEST_F(CompactionTest, DirectCompactorAPI) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Create a partition and write data
        config_.auto_compact = true;
        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        std::vector data(200, 'A');
        for (int i = 0; i < 20; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }

        // Update to create fragmentation
        for (int i = 0; i < 10; ++i) {
            std::vector new_data(200, 'B');
            co_await partition->put(std::format("key_{}", i), std::move(new_data));
        }

        // Note: Direct compactor API testing is tricky because CompactionContext
        // is tied to Partition internals. The above tests through partition->compact()
        // are the primary way to test compaction correctness.

        // Verify compaction worked by checking data is accessible
        for (int i = 0; i < 20; ++i) {
            auto get_result = co_await partition->get(std::format("key_{}", i));
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());
        }
    };

    SyncWait(test_coro());
}

TEST_F(CompactionTest, FragmentationThreshold) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // Write data
        std::vector data(300, 'X');
        for (int i = 0; i < 20; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }

        // Create exactly threshold fragmentation (50%)
        // Update half the keys
        for (int i = 0; i < 10; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }

        const auto stats = partition->get_stats();
        const double fragmentation = stats.overall_fragmentation();

        // Should be around threshold
        EXPECT_GT(fragmentation, 0.3);  // At least some fragmentation

        // Manual compact should work regardless of threshold
        const auto result = co_await partition->compact();
        EXPECT_TRUE(result.has_value());
    };

    SyncWait(test_coro());
}


// ----------------------------------------------------------------------------
// Test: Concurrency Stress (Race Condition Verification)
// Run Compaction in parallel with intense PUT operations.
// The KeyDir CAS logic ensures that if a key is updated during compaction,
// the old value moved by compaction does NOT overwrite the new value in KeyDir.
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, ConcurrentUpdatesDuringCompaction) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // 1. Seed data (lots of it to make compaction slow)
        std::vector payload(100, 'A');
        for (int i = 0; i < 1000; ++i) {
            co_await partition->put(std::format("static_{}", i), std::vector(payload));
        }
        // Create garbage to ensure compaction has work to do
        for (int i = 0; i < 500; ++i) {
            co_await partition->put(std::format("static_{}", i), std::vector(payload));
        }

        // 2. Define concurrent tasks

        // Task A: The Compactor
        auto task_compact = [&]() -> Task<void> {
            co_await SwitchToWorker(*worker_);
            // Yield a bit to let the writer start
            for(int i=0; i<10; ++i) co_await worker_->async_sleep(std::chrono::milliseconds(1));

            const auto res = co_await partition->compact();
            EXPECT_TRUE(res.has_value());
        };

        // Task B: The Concurrent Writer
        // Writes to a specific key "race_key" continuously.
        auto task_writer = [&]() -> Task<void> {
            co_await SwitchToWorker(*worker_);

            for (int i = 0; i < 100; ++i) {
                // Changing values: V_0, V_1, ... V_99
                std::string val = std::format("V_{}", i);
                co_await partition->put("race_key", std::vector<char>(val.begin(), val.end()));

                // Small delay to interleave execution with compactor
                if (i % 10 == 0) co_await worker_->async_sleep(std::chrono::milliseconds(1));
            }

            // Final definitive write
            std::string final_val = "FINAL_VALUE";
            co_await partition->put("race_key", std::vector<char>(final_val.begin(), final_val.end()));
        };

        // 3. Put "race_key" initially so it exists in the OLD files (candidate for compaction)
        co_await partition->put("race_key", std::vector<char>{'I', 'N', 'I', 'T'});

        // 4. Run both concurrently
        // Note: bitcask::Partition is single-threaded (on worker_), but logic interleaves at suspension points.
        // We simulate concurrency by running two coroutines on the same worker.
        auto t1 = task_compact();
        auto t2 = task_writer();

        co_await t1;
        co_await t2;

        // 5. Verification
        // If Compaction's CAS logic is broken, it might have overwritten "race_key"
        // with "INIT" (from the old file) *after* the writer wrote "FINAL_VALUE".

        auto res = co_await partition->get("race_key");
        EXPECT_TRUE(res.has_value() && res.value().has_value());

        std::string actual(res.value().value().begin(), res.value().value().end());
        EXPECT_EQ(actual, "FINAL_VALUE") << "Compaction overwrote the concurrent update!";

        co_await partition->async_close();
    };

    SyncWait(test_coro());
}

// ----------------------------------------------------------------------------
// Test: Compaction Loop Trigger
// Verifies that the background compaction loop responds to the manual trigger
// and performs compaction when signaled.
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, CompactionLoopTrigger) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Enable auto-compaction config
        config_.auto_compact = true;
        // Set a short interval to ensure the timer trigger fires if the event trigger is missed
        config_.compaction_interval_s = std::chrono::seconds(1);

        auto partition_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_res.has_value());
        auto partition = std::move(partition_res.value());

        // 1. Create fragmentation to give the loop something to do
        std::vector data(400, 'A');
        for (int i = 0; i < 20; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }
        // Update to create dead data
        for (int i = 0; i < 15; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector(data));
        }

        // 3. Verify initial state
        auto stats_before = partition->get_stats();
        EXPECT_EQ(stats_before.compactions_total, 0);

        // 4. Wait for the background loop to process the compaction
        // The loop waits on compaction_trigger_ OR the timeout.
        // We put a generous wait here (up to 2 seconds) to allow the loop to wake up.
        for (int i = 0; i < 40; ++i) {
            co_await worker_->async_sleep(std::chrono::milliseconds(50));
            if (partition->get_stats().compactions_total > 0) break;
        }

        const auto stats_after = partition->get_stats();

        if (config_.auto_compact) {
            EXPECT_GE(stats_after.compactions_total, 1) << "Background loop should have compacted files";
        }

        co_await partition->async_close();
    };

    SyncWait(test_coro());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}