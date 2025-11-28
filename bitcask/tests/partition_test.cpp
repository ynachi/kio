#include <gtest/gtest.h>
#include <filesystem>
#include "bitcask/include/partition.h"
#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

// ============================================================================
// Partition Core Logic Tests
// ============================================================================

class PartitionTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;
    BitcaskConfig config_;

    void SetUp() override {
        alog::configure(1024, LogLevel::Debug);
        test_dir_ = std::filesystem::temp_directory_path() / "bitcask_partition_test/";
        std::filesystem::remove_all(test_dir_);
        // create the partition dir too
        std::filesystem::create_directories(test_dir_ / "partition_0");

        config_.directory = test_dir_;
        config_.max_file_size = 10 * 1024;  // 10KB for faster rotation in tests
        config_.sync_on_write = false; // faster testing
        config_.auto_compact = false;

        // Create worker
        WorkerConfig worker_config;
        worker_config.uring_queue_depth = 128;
        worker_ = std::make_unique<Worker>(0, worker_config);

        // Start a worker thread
        worker_thread_ = std::thread([this]() {
            worker_->loop_forever();
        });

        worker_->wait_ready();
    }

    void TearDown() override {
        // Proper shutdown sequence
        if (worker_) {
            (void)worker_->request_stop();
            worker_->wait_shutdown();
        }

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        worker_.reset();

        // Cleanup
        std::filesystem::remove_all(test_dir_);
    }
};

// ============================================================================
// Basic Operations Tests
// ============================================================================

TEST_F(PartitionTest, Creation) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition_result = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(partition_result.has_value());

        if (partition_result.has_value()) {
            const auto& partition = partition_result.value();

            // Verify the partition directory was created
            const auto partition_dir = test_dir_ / "partition_0";
            EXPECT_TRUE(std::filesystem::exists(partition_dir));

            // Verify stats are initialized
            const auto& stats = partition->get_stats();
            EXPECT_EQ(stats.puts_total, 0);
            EXPECT_EQ(stats.gets_total, 0);
            EXPECT_EQ(stats.deletes_total, 0);
        }
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, BasicPut) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Put a key-value pair
        std::string key = "test_key";
        std::vector value = {'v', 'a', 'l', 'u', 'e'};

        const auto put_result = co_await partition->put(std::string(key), std::vector(value));
        EXPECT_TRUE(put_result.has_value());

        // Verify stats
        const auto& stats = partition->get_stats();
        EXPECT_EQ(stats.puts_total, 1);
        EXPECT_EQ(stats.data_files.size(), 1);
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, BasicGet) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        std::string key = "get_key";
        std::vector value = {'g', 'e', 't', '_', 'v', 'a', 'l'};

        // Put
        co_await partition->put(std::string(key), std::vector(value));
        const DataEntry d{std::string(key), std::vector(value)};
        const auto check = d.serialize();
        std::cout << "serialized data size " << check.size() << std::endl;
        ALOG_INFO("Data content from test: {}", std::string_view(check.data(), check.size()));

        // Get
        const auto get_result = co_await partition->get(key);
        ALOG_INFO("{}",get_result.error());
        EXPECT_TRUE(get_result.has_value());

        if (get_result.has_value()) {
            const auto& retrieved = get_result.value();
            EXPECT_TRUE(retrieved.has_value());

            if (retrieved.has_value()) {
                EXPECT_EQ(retrieved.value(), value);
            }
        }

        // Verify stats
        const auto& stats = partition->get_stats();
        EXPECT_EQ(stats.gets_total, 1);
        EXPECT_EQ(stats.gets_miss_total, 0);
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, GetMissing) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        const auto get_result = co_await partition->get("non_existent_key");
        EXPECT_TRUE(get_result.has_value());

        if (get_result.has_value()) {
            const auto& retrieved = get_result.value();
            EXPECT_FALSE(retrieved.has_value());
        }

        // Verify stats
        const auto& stats = partition->get_stats();
        EXPECT_EQ(stats.gets_total, 1);
        EXPECT_EQ(stats.gets_miss_total, 1);
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, UpdateKey) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        std::string key = "update_key";

        // First put
        std::vector value1 = {'v', '1'};
        co_await partition->put(std::string(key), std::vector(value1));

        // Update
        std::vector value2 = {'v', '2', '_', 'u', 'p', 'd', 'a', 't', 'e', 'd'};
        co_await partition->put(std::string(key), std::vector(value2));

        // Get should return the updated value
        const auto get_result = co_await partition->get(key);
        EXPECT_TRUE(get_result.has_value());
        EXPECT_EQ(get_result.value().value(), value2);

        if (get_result.has_value() && get_result.value().has_value()) {
            EXPECT_EQ(get_result.value().value(), value2);
        }

        // Stats should show 2 puts
        const auto& stats = partition->get_stats();
        EXPECT_EQ(stats.puts_total, 2);
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, DeleteKey) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        std::string key = "delete_me";
        std::vector value = {'v', 'a', 'l'};

        // Put
        co_await partition->put(std::string(key), std::vector(value));

        // Verify exists
        const auto get1 = co_await partition->get(key);
        EXPECT_TRUE(get1.has_value() && get1.value().has_value());

        // Delete
        const auto del_result = co_await partition->del(key);
        EXPECT_TRUE(del_result.has_value());

        // Verify deleted
        const auto get2 = co_await partition->get(key);
        EXPECT_TRUE(get2.has_value());
        EXPECT_FALSE(get2.value().has_value());  // Should be nullptr

        // Verify stats
        const auto& stats = partition->get_stats();
        EXPECT_EQ(stats.deletes_total, 1);
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, DeleteNonExistent) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Delete it non-existent key should succeed (idempotent)
        const auto del_result = co_await partition->del("ghost_key");
        EXPECT_TRUE(del_result.has_value());

        const auto& stats = partition->get_stats();
        EXPECT_EQ(stats.deletes_total, 1);
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, MultipleKeys) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Put multiple keys
        for (int i = 0; i < 100; ++i) {
            std::string key = std::format("key_{}", i);
            std::string val_str = std::format("value_{}", i);
            std::vector value(val_str.begin(), val_str.end());

            auto put_result = co_await partition->put(std::move(key), std::move(value));
            EXPECT_TRUE(put_result.has_value());
        }

        // Get all keys
        for (int i = 0; i < 100; ++i) {
            std::string key = std::format("key_{}", i);
            std::string expected_val = std::format("value_{}", i);
            std::vector expected(expected_val.begin(), expected_val.end());

            auto get_result = co_await partition->get(key);
            EXPECT_TRUE(get_result.has_value());

            if (get_result.has_value() && get_result.value().has_value()) {
                EXPECT_EQ(get_result.value().value(), expected);
            }
        }

        const auto& stats = partition->get_stats();
        EXPECT_EQ(stats.puts_total, 100);
        EXPECT_EQ(stats.gets_total, 100);
        EXPECT_EQ(stats.gets_miss_total, 0);
    };

    SyncWait(test_coro());
}

// ============================================================================
// File Rotation Tests
// ============================================================================
TEST_F(PartitionTest, FileRotation) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Write data larger than max_file_size
        std::vector large_value(config_.max_file_size / 2, 'X');

        // This should cause rotation after 3 writes
        co_await partition->put("key1", std::vector(large_value));
        co_await partition->put("key2", std::vector(large_value));
        co_await partition->put("key3", std::vector(large_value));

        const auto& stats = partition->get_stats();

        // Should have multiple files due to rotation
        EXPECT_GE(stats.data_files.size(), 2);
        EXPECT_GE(stats.file_rotations_total, 1);

        // Verify all keys are still readable
        const auto get1 = co_await partition->get("key1");
        const auto get2 = co_await partition->get("key2");
        const auto get3 = co_await partition->get("key3");

        EXPECT_TRUE(get1.has_value() && get1.value().has_value());
        EXPECT_TRUE(get2.has_value() && get2.value().has_value());
        EXPECT_TRUE(get3.has_value() && get3.value().has_value());
    };

    SyncWait(test_coro());
}

// ============================================================================
// Recovery Tests
// ============================================================================

TEST_F(PartitionTest, SimpleRecovery) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write data
        {
            const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            co_await partition->put("recover_key1", std::vector{'v', '1'});
            co_await partition->put("recover_key2", std::vector{'v', '2'});
            co_await partition->put("recover_key3", std::vector{'v', '3'});

            // Partition goes out of scope, files are closed
        }

        // Phase 2: Recover
        {
            const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            // Verify all keys recovered
            const auto get1 = co_await partition->get("recover_key1");
            const auto get2 = co_await partition->get("recover_key2");
            const auto get3 = co_await partition->get("recover_key3");

            EXPECT_TRUE(get1.has_value() && get1.value().has_value());
            EXPECT_TRUE(get2.has_value() && get2.value().has_value());
            EXPECT_TRUE(get3.has_value() && get3.value().has_value());

            if (get1.value().has_value()) {
                EXPECT_EQ(get1.value().value(), std::vector({'v', '1'}));
            }
        }
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, RecoveryWithUpdates) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write and update
        {
            const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            co_await partition->put("key", std::vector{'o', 'l', 'd'});
            co_await partition->put("key", std::vector{'n', 'e', 'w'});
            co_await partition->async_close();
        }

        // Phase 2: Recover
        {
            const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            const auto get_result = co_await partition->get("key");
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());

            if (get_result.value().has_value()) {
                // Should get the latest value
                EXPECT_EQ(get_result.value().value(), std::vector({'n', 'e', 'w'}));
            }
        }
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, RecoveryWithDeletions) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write and delete
        {
            const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            co_await partition->put("deleted_key", std::vector{'v', 'a', 'l'});
            co_await partition->put("kept_key", std::vector{'k', 'e', 'p', 't'});
            co_await partition->del("deleted_key");
            co_await partition->async_close();
        }

        // Phase 2: Recover
        {
            const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            const auto get_deleted = co_await partition->get("deleted_key");
            const auto get_kept = co_await partition->get("kept_key");

            // Deleted key should not be found
            EXPECT_TRUE(get_deleted.has_value());
            EXPECT_FALSE(get_deleted.value().has_value());

            // Kept key should exist
            EXPECT_TRUE(get_kept.has_value() && get_kept.value().has_value());
        }
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, RecoveryMultipleFiles) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write enough to trigger rotation
        {
            const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            std::vector large_value(config_.max_file_size / 2, 'X');

            for (int i = 0; i < 5; ++i) {
                std::string key = std::format("big_key_{}", i);
                co_await partition->put(std::move(key), std::vector(large_value));
            }

            const auto& stats = partition->get_stats();
            EXPECT_GE(stats.data_files.size(), 2) << "Should have multiple files";
            co_await partition->async_close();
        }

        // Phase 2: Recover from multiple files
        {
            auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

            // Verify all keys recovered
            for (int i = 0; i < 5; ++i) {
                std::string key = std::format("big_key_{}", i);
                auto get_result = co_await partition->get(key);

                EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                    << "Key " << key << " should be recovered";
            }
        }
    };

    SyncWait(test_coro());
}

// ============================================================================
// Stats Tests
// ============================================================================

TEST_F(PartitionTest, StatsAccuracy) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Perform operations
        co_await partition->put("key1", std::vector{'v', '1'});
        co_await partition->put("key2", std::vector{'v', '2'});
        co_await partition->put("key1", std::vector{'v', '1', '_', 'u', 'p', 'd'});

        co_await partition->get("key1");
        co_await partition->get("key2");
        co_await partition->get("missing");  // Miss

        co_await partition->del("key2");

        const auto& stats = partition->get_stats();

        EXPECT_EQ(stats.puts_total, 3);
        EXPECT_EQ(stats.gets_total, 3);
        EXPECT_EQ(stats.gets_miss_total, 1);
        EXPECT_EQ(stats.deletes_total, 1);
    };

    SyncWait(test_coro());
}

TEST_F(PartitionTest, FragmentationCalculation) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Write keys
        co_await partition->put("key1", std::vector(100, 'X'));
        co_await partition->put("key2", std::vector(100, 'Y'));
        co_await partition->put("key3", std::vector(100, 'Z'));

        // Update key1 (creates dead data)
        co_await partition->put("key1", std::vector(100, 'A'));

        // Delete key2 (creates dead data)
        co_await partition->del("key2");

        const auto& stats = partition->get_stats();

        // Should have some fragmentation now
        const double fragmentation = stats.overall_fragmentation();
        EXPECT_GT(fragmentation, 0.0) << "Should have fragmentation after updates/deletes";
        EXPECT_LT(fragmentation, 1.0) << "Fragmentation should be < 100%";
    };

    SyncWait(test_coro());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}