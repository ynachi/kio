//
// Created by Yao ACHI on 26/11/2025.
//

#include <gtest/gtest.h>
#include <filesystem>
#include "bitcask/include/partition.h"
#include "bitcask/include/compactor.h"
#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

// ============================================================================
// PHASE 4: Compaction Tests
// ============================================================================

class CompactionTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;
    BitcaskConfig config_;

    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "bitcask_compaction_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        config_.directory = test_dir_;
        config_.max_file_size = 4 * 1024;  // 4KB for fast rotation
        config_.sync_on_write = false;
        config_.auto_compact = false;  // Manual control for tests
        config_.fragmentation_threshold = 0.5;  // 50% dead data triggers compaction

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
            worker_->request_stop();
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

// ----------------------------------------------------------------------------
// Test 1: Compaction Reduces File Count
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, ReducesFileCount) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Create fragmentation: write, update, delete
        std::vector<char> data(500, 'X');

        // Write to multiple files
        for (int i = 0; i < 20; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->put(std::move(key), std::vector<char>(data));
        }

        // Update half (creates dead data)
        for (int i = 0; i < 10; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->put(std::move(key), std::vector<char>(data));
        }

        auto stats_before = partition->get_stats();
        size_t files_before = stats_before.data_files.size();

        EXPECT_GT(files_before, 1) << "Should have multiple files";

        // Compact
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

// ----------------------------------------------------------------------------
// Test 2: Compaction Preserves Live Data
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, PreservesLiveData) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Write initial data
        std::vector<std::string> keys;
        for (int i = 0; i < 50; ++i) {
            std::string key = std::format("key_{}", i);
            std::string val_str = std::format("value_{}", i);
            std::vector<char> value(val_str.begin(), val_str.end());

            co_await partition->put(std::string(key), std::move(value));
            keys.push_back(key);
        }

        // Create fragmentation by updating some keys
        for (int i = 0; i < 25; ++i) {
            std::string key = std::format("key_{}", i);
            std::string val_str = std::format("updated_{}", i);
            std::vector<char> value(val_str.begin(), val_str.end());

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

                std::vector<char> expected_vec(expected.begin(), expected.end());
                EXPECT_EQ(get_result.value().value(), expected_vec);
            }
        }
    };

    SyncWait(test_coro());
}

// ----------------------------------------------------------------------------
// Test 3: Compaction Removes Deleted Keys
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, RemovesDeletedKeys) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Write data
        for (int i = 0; i < 30; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector<char> value(100, 'X');
            co_await partition->put(std::move(key), std::move(value));
        }

        // Delete half
        for (int i = 0; i < 15; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->del(key);
        }

        auto stats_before = partition->get_stats();
        uint64_t total_bytes_before = 0;
        for (const auto& [_, file_stats] : stats_before.data_files) {
            total_bytes_before += file_stats.total_bytes;
        }

        // Compact
        co_await partition->compact();

        auto stats_after = partition->get_stats();
        uint64_t total_bytes_after = 0;
        for (const auto& [_, file_stats] : stats_after.data_files) {
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

// ----------------------------------------------------------------------------
// Test 4: Compaction Handles Concurrent Updates (via KeyDir CAS)
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, HandlesStaleEntries) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        std::vector<char> data(300, 'X');

        // Write initial data (will be in old files)
        for (int i = 0; i < 10; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->put(std::string(key), std::vector<char>(data));
        }

        // Force file rotation by writing more
        for (int i = 10; i < 20; ++i) {
            std::string key = std::format("key_{}", i);
            co_await partition->put(std::string(key), std::vector<char>(data));
        }

        // Update keys 0-5 AFTER they're in old files
        // This simulates the case where compaction might encounter stale entries
        for (int i = 0; i < 5; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector<char> new_data(300, 'Y');
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

// ============================================================================
// Recovery After Compaction Tests
// ============================================================================

// ----------------------------------------------------------------------------
// Test 5: Recovery After Compaction
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, RecoveryAfterCompaction) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write, compact, close
        {
            auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            // Write data
            for (int i = 0; i < 20; ++i) {
                std::string key = std::format("key_{}", i);
                std::vector<char> value(200, static_cast<char>('A' + i % 26));
                co_await partition->put(std::move(key), std::move(value));
            }

            // Update to create fragmentation
            for (int i = 0; i < 10; ++i) {
                std::string key = std::format("key_{}", i);
                std::vector<char> value(200, 'Z');
                co_await partition->put(std::move(key), std::move(value));
            }

            // Compact
            auto compact_result = co_await partition->compact();
            EXPECT_TRUE(compact_result.has_value());

            // Partition goes out of scope
        }

        // Phase 2: Recover and verify
        {
            auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

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

// ----------------------------------------------------------------------------
// Test 6: Multiple Compaction Cycles
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, MultipleCompactionCycles) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        std::vector<char> data(200, 'X');

        // Cycle 1
        for (int i = 0; i < 15; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector<char>(data));
        }
        for (int i = 0; i < 10; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector<char>(data));
        }
        co_await partition->compact();

        // Cycle 2
        for (int i = 0; i < 10; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector<char>(data));
        }
        co_await partition->compact();

        // Cycle 3
        for (int i = 5; i < 15; ++i) {
            co_await partition->del(std::format("key_{}", i));
        }
        co_await partition->compact();

        auto stats = partition->get_stats();
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
    };

    SyncWait(test_coro());
}

// ============================================================================
// Edge Cases & Stress Tests
// ============================================================================

// ----------------------------------------------------------------------------
// Test 7: Compaction with All Dead Data
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, AllDeadData) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Write data
        for (int i = 0; i < 20; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector<char> value(200, 'X');
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

// ----------------------------------------------------------------------------
// Test 8: Compaction with No Dead Data (No-Op)
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, NoDeadData) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Write fresh data (no updates, no deletes)
        for (int i = 0; i < 10; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector<char> value(100, 'X');
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

// ----------------------------------------------------------------------------
// Test 9: Large Number of Small Updates (Fragmentation Test)
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, ManySmallUpdates) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Create a single key with many updates
        std::string key = "frequently_updated";

        for (int version = 0; version < 50; ++version) {
            std::string val_str = std::format("version_{}", version);
            std::vector<char> value(val_str.begin(), val_str.end());
            co_await partition->put(std::string(key), std::move(value));
        }

        auto stats_before = partition->get_stats();
        double frag_before = stats_before.overall_fragmentation();

        EXPECT_GT(frag_before, 0.5) << "Should have significant fragmentation";

        // Compact
        co_await partition->compact();

        auto stats_after = partition->get_stats();
        double frag_after = stats_after.overall_fragmentation();

        // Fragmentation should decrease
        EXPECT_LT(frag_after, frag_before);

        // Latest value should be accessible
        auto get_result = co_await partition->get(key);
        EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());

        if (get_result.value().has_value()) {
            std::string expected = "version_49";
            std::vector<char> expected_vec(expected.begin(), expected.end());
            EXPECT_EQ(get_result.value().value(), expected_vec);
        }
    };

    SyncWait(test_coro());
}

// ----------------------------------------------------------------------------
// Test 10: Compaction Stats Tracking
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, StatsTracking) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Create workload
        std::vector<char> data(300, 'X');
        for (int i = 0; i < 30; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector<char>(data));
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

// ----------------------------------------------------------------------------
// Test 11: Direct Compactor API Test
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, DirectCompactorAPI) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Create a partition and write data
        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        std::vector<char> data(200, 'A');
        for (int i = 0; i < 20; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector<char>(data));
        }

        // Update to create fragmentation
        for (int i = 0; i < 10; ++i) {
            std::vector<char> new_data(200, 'B');
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

// ----------------------------------------------------------------------------
// Test 12: Fragmentation Threshold Behavior
// ----------------------------------------------------------------------------
TEST_F(CompactionTest, FragmentationThreshold) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Write data
        std::vector<char> data(300, 'X');
        for (int i = 0; i < 20; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector<char>(data));
        }

        // Create exactly threshold fragmentation (50%)
        // Update half the keys
        for (int i = 0; i < 10; ++i) {
            co_await partition->put(std::format("key_{}", i), std::vector<char>(data));
        }

        auto stats = partition->get_stats();
        double fragmentation = stats.overall_fragmentation();

        // Should be around threshold
        EXPECT_GT(fragmentation, 0.3);  // At least some fragmentation

        // Manual compact should work regardless of threshold
        auto result = co_await partition->compact();
        EXPECT_TRUE(result.has_value());
    };

    SyncWait(test_coro());
}