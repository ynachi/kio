//
// Created by Yao ACHI on 09/01/2026.
//
//
// Integration tests for Partition
//
// Tests the Partition component with real disk I/O:
// - CRUD operations (Put, Get, Delete)
// - Recovery from data files and hint files
// - File rotation
// - Compaction
// - Stats tracking
//

#include <gtest/gtest.h>
#include <filesystem>

#include "bitcask/include/partition.h"
#include "kio/core/worker.h"
#include "kio/sync/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class PartitionTest : public ::testing::Test
{
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;
    BitcaskConfig config_;

    void SetUp() override
    {
        alog::Configure(1024, LogLevel::kDisabled);

        test_dir_ = std::filesystem::temp_directory_path() / "partition_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        // Ensure the partition subdirectory exists.
        std::filesystem::create_directories(test_dir_ / "partition_0");

        config_.directory = test_dir_;
        config_.max_file_size = 10 * 1024;  // 10KB for faster rotation in tests
        config_.sync_on_write = false;      // faster testing
        config_.auto_compact = false;
        // Lower the threshold to ensure fragmentation triggers compaction easily in small tests
        config_.fragmentation_threshold = 0.4;

        // Create worker
        WorkerConfig worker_config;
        worker_config.uring_queue_depth = 128;
        worker_ = std::make_unique<Worker>(0, worker_config);

        // Start a worker thread
        worker_thread_ = std::thread([this]() {
            worker_->LoopForever();
        });

        worker_->WaitReady();
    }

    void TearDown() override
    {
        // Proper shutdown sequence
        if (worker_)
        {
            (void)worker_->RequestStop();
            worker_->WaitShutdown();
        }

        if (worker_thread_.joinable())
        {
            worker_thread_.join();
        }

        worker_.reset();

        // Cleanup
        std::filesystem::remove_all(test_dir_);
    }
};

// ============================================================================
// Basic CRUD Operations
// ============================================================================

TEST_F(PartitionTest, PutAndGet)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        EXPECT_TRUE(open_res.has_value()) << "Open failed: " << open_res.error();
        if (!open_res.has_value()) co_return;

        auto partition = std::move(open_res.value());

        // Put
        auto put_result = co_await partition->Put("test_key", {'v', 'a', 'l', 'u', 'e'});
        EXPECT_TRUE(put_result.has_value()) << "Put failed: " << put_result.error();

        // Get
        auto get_result = co_await partition->Get("test_key");
        EXPECT_TRUE(get_result.has_value()) << "Get failed (IO error): " << get_result.error();

        if (get_result.has_value()) {
            EXPECT_TRUE(get_result.value().has_value()) << "Get failed (Key not found)";
            if (get_result.value().has_value()) {
                 EXPECT_EQ(get_result.value().value(), (std::vector<char>{'v', 'a', 'l', 'u', 'e'}));
            }
        }

        // Stats
        auto stats = partition->GetStats();
        EXPECT_EQ(stats.puts_total, 1);
        EXPECT_EQ(stats.gets_total, 1);
    };

    SyncWait(test());
}

TEST_F(PartitionTest, GetMissing)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        EXPECT_TRUE(open_res.has_value()) << "Open failed: " << open_res.error();
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        auto result = co_await partition->Get("nonexistent");
        EXPECT_TRUE(result.has_value()) << result.error();
        if (result.has_value()) {
            EXPECT_FALSE(result.value().has_value());  // Key not found
        }

        auto stats = partition->GetStats();
        EXPECT_EQ(stats.gets_miss_total, 1);
    };

    SyncWait(test());
}

TEST_F(PartitionTest, UpdateOverwritesValue)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        EXPECT_TRUE(open_res.has_value()) << "Open failed: " << open_res.error();
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        EXPECT_TRUE((co_await partition->Put("key", {'o', 'l', 'd'})).has_value());
        EXPECT_TRUE((co_await partition->Put("key", {'n', 'e', 'w'})).has_value());

        auto result = co_await partition->Get("key");
        EXPECT_TRUE(result.has_value()) << result.error();
        if (result.has_value() && result.value().has_value()) {
             EXPECT_EQ(result.value().value(), (std::vector<char>{'n', 'e', 'w'}));
        } else {
             ADD_FAILURE() << "Failed to retrieve updated key";
        }

        auto stats = partition->GetStats();
        EXPECT_EQ(stats.puts_total, 2);
    };

    SyncWait(test());
}

TEST_F(PartitionTest, DeleteRemovesKey)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        EXPECT_TRUE(open_res.has_value()) << "Open failed: " << open_res.error();
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        co_await partition->Put("delete_me", {'v', 'a', 'l'});

        // Verify exists
        auto get1 = co_await partition->Get("delete_me");
        EXPECT_TRUE(get1.has_value());
        if (get1.has_value()) {
            EXPECT_TRUE(get1.value().has_value());
        }

        // Delete
        auto del_result = co_await partition->Del("delete_me");
        EXPECT_TRUE(del_result.has_value());

        // Verify gone
        auto get2 = co_await partition->Get("delete_me");
        EXPECT_TRUE(get2.has_value());
        if (get2.has_value()) {
            EXPECT_FALSE(get2.value().has_value());
        }

        auto stats = partition->GetStats();
        EXPECT_EQ(stats.deletes_total, 1);
    };

    SyncWait(test());
}

// ============================================================================
// File Rotation
// ============================================================================

TEST_F(PartitionTest, FileRotationOnSizeLimit)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        EXPECT_TRUE(open_res.has_value()) << "Open failed: " << open_res.error();
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        // Write enough data to trigger rotation (max_file_size = 10KB)
        std::vector<char> large_value(config_.max_file_size / 2, 'X');

        EXPECT_TRUE((co_await partition->Put("key1", std::vector<char>(large_value))).has_value());
        EXPECT_TRUE((co_await partition->Put("key2", std::vector<char>(large_value))).has_value());
        EXPECT_TRUE((co_await partition->Put("key3", std::vector<char>(large_value))).has_value());

        auto stats = partition->GetStats();
        EXPECT_GE(stats.data_files.size(), 2) << "Should have rotated to new file";
        EXPECT_GE(stats.file_rotations_total, 1);

        // All keys should still be readable
        auto r1 = co_await partition->Get("key1");
        auto r2 = co_await partition->Get("key2");
        auto r3 = co_await partition->Get("key3");

        EXPECT_TRUE(r1.has_value());
        EXPECT_TRUE(r2.has_value());
        EXPECT_TRUE(r3.has_value());

        if (r1.has_value()) EXPECT_TRUE(r1.value().has_value());
        if (r2.has_value()) EXPECT_TRUE(r2.value().has_value());
        if (r3.has_value()) EXPECT_TRUE(r3.value().has_value());
    };

    SyncWait(test());
}

// ============================================================================
// Recovery
// ============================================================================

TEST_F(PartitionTest, RecoveryFromDataFiles)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write data
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            EXPECT_TRUE(open_res.has_value()) << "Open failed: " << open_res.error();
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            co_await partition->Put("key1", {'v', '1'});
            co_await partition->Put("key2", {'v', '2'});
            co_await partition->Put("key3", {'v', '3'});

            co_await partition->AsyncClose();
        }

        // Phase 2: Reopen and verify recovery
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            EXPECT_TRUE(open_res.has_value()) << "Open failed (Recovery): " << open_res.error();
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            auto get1 = co_await partition->Get("key1");
            auto get2 = co_await partition->Get("key2");
            auto get3 = co_await partition->Get("key3");

            EXPECT_TRUE(get1.has_value());
            EXPECT_TRUE(get2.has_value());
            EXPECT_TRUE(get3.has_value());

            if (get1.has_value() && get1.value().has_value())
                EXPECT_EQ(get1.value().value(), (std::vector<char>{'v', '1'}));

            if (get2.has_value() && get2.value().has_value())
                EXPECT_EQ(get2.value().value(), (std::vector<char>{'v', '2'}));

            if (get3.has_value() && get3.value().has_value())
                EXPECT_EQ(get3.value().value(), (std::vector<char>{'v', '3'}));
        }
    };

    SyncWait(test());
}

TEST_F(PartitionTest, RecoveryRespectsUpdates)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write and update
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            co_await partition->Put("key", {'o', 'l', 'd'});
            co_await partition->Put("key", {'n', 'e', 'w'});

            co_await partition->AsyncClose();
        }

        // Phase 2: Verify latest value survives
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            auto result = co_await partition->Get("key");
            EXPECT_TRUE(result.has_value());
            if (result.has_value() && result.value().has_value()) {
                 EXPECT_EQ(result.value().value(), (std::vector<char>{'n', 'e', 'w'}));
            }
        }
    };

    SyncWait(test());
}

TEST_F(PartitionTest, RecoveryRespectsDeletions)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write and delete
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            co_await partition->Put("deleted", {'v', 'a', 'l'});
            co_await partition->Put("kept", {'k', 'e', 'p', 't'});
            co_await partition->Del("deleted");

            co_await partition->AsyncClose();
        }

        // Phase 2: Verify deletion persists
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            auto get_deleted = co_await partition->Get("deleted");
            auto get_kept = co_await partition->Get("kept");

            EXPECT_TRUE(get_deleted.has_value());
            EXPECT_TRUE(get_kept.has_value());

            if (get_deleted.has_value()) EXPECT_FALSE(get_deleted.value().has_value());
            if (get_kept.has_value()) EXPECT_TRUE(get_kept.value().has_value());
        }
    };

    SyncWait(test());
}

TEST_F(PartitionTest, RecoveryAcrossMultipleFiles)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write enough to span multiple files
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            if (!open_res.has_value()) co_return;
            const auto partition = std::move(open_res.value());

            std::vector const large_value(config_.max_file_size / 2, 'X');

            for (int i = 0; i < 5; ++i)
            {
                co_await partition->Put(std::format("big_key_{}", i), std::vector<char>(large_value));
            }

            auto stats = partition->GetStats();
            EXPECT_GE(stats.data_files.size(), 2);

            co_await partition->AsyncClose();
        }

        // Phase 2: Recover and verify
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            for (int i = 0; i < 5; ++i)
            {
                auto result = co_await partition->Get(std::format("big_key_{}", i));
                EXPECT_TRUE(result.has_value());
                if (result.has_value()) {
                    EXPECT_TRUE(result.value().has_value()) << "Key big_key_" << i << " should be recovered";
                }
            }
        }
    };

    SyncWait(test());
}

// ============================================================================
// Compaction
// ============================================================================

TEST_F(PartitionTest, CompactionPreservesLiveData)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        // Write initial data
        for (int i = 0; i < 50; ++i)
        {
            std::string val = std::format("value_{}", i);
            co_await partition->Put(std::format("key_{}", i), std::vector<char>(val.begin(), val.end()));
        }

        // Update half (creates dead data)
        for (int i = 0; i < 25; ++i)
        {
            std::string val = std::format("updated_{}", i);
            co_await partition->Put(std::format("key_{}", i), std::vector<char>(val.begin(), val.end()));
        }

        // Compact
        auto compact_result = co_await partition->Compact();
        EXPECT_TRUE(compact_result.has_value());

        // Verify all data still accessible with correct values
        for (int i = 0; i < 50; ++i)
        {
            auto result = co_await partition->Get(std::format("key_{}", i));
            EXPECT_TRUE(result.has_value()) << "key_" << i << " read failed";

            if (result.has_value()) {
                EXPECT_TRUE(result.value().has_value()) << "key_" << i << " should exist";
                if (result.value().has_value()) {
                    std::string expected = (i < 25) ? std::format("updated_{}", i) : std::format("value_{}", i);
                    std::vector<char> expected_vec(expected.begin(), expected.end());
                    EXPECT_EQ(result.value().value(), expected_vec);
                }
            }
        }
    };

    SyncWait(test());
}

TEST_F(PartitionTest, CompactionRemovesDeletedKeys)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        EXPECT_TRUE(open_res.has_value()) << "Open failed: " << open_res.error();
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        // Write data with LARGE values to force file rotation
        // max_file_size = 10KB, so ~500 bytes * 30 entries = 15KB > 10KB → rotation!
        for (int i = 0; i < 30; ++i)
        {
            co_await partition->Put(std::format("key_{}", i), std::vector<char>(500, 'X'));
        }

        // Verify we have sealed files (rotation happened)
        auto stats_mid = partition->GetStats();
        EXPECT_GE(stats_mid.data_files.size(), 2)
            << "Should have rotated files. Total files: " << stats_mid.data_files.size();

        // Delete half
        for (int i = 0; i < 15; ++i)
        {
            co_await partition->Del(std::format("key_{}", i));
        }

        auto stats_before = partition->GetStats();

        // Compact
        auto res = co_await partition->Compact();
        EXPECT_TRUE(res.has_value()) << res.error();

        auto stats_after = partition->GetStats();
        EXPECT_GT(stats_after.bytes_reclaimed_total, 0)
            << "Should have reclaimed bytes. Files before: " << stats_before.data_files.size()
            << ", Fragmentation: " << stats_before.OverallFragmentation();

        // Verify deleted keys stay deleted
        for (int i = 0; i < 15; ++i)
        {
            auto result = co_await partition->Get(std::format("key_{}", i));
            EXPECT_TRUE(result.has_value());
            if (result.has_value()) {
                EXPECT_FALSE(result.value().has_value());
            }
        }

        // Verify kept keys still exist
        for (int i = 15; i < 30; ++i)
        {
            auto result = co_await partition->Get(std::format("key_{}", i));
            EXPECT_TRUE(result.has_value());
            if (result.has_value()) {
                 EXPECT_TRUE(result.value().has_value());
            }
        }
    };

    SyncWait(test());
}

TEST_F(PartitionTest, CompactionDataSurvivesRestart)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        // Phase 1: Write, compact, close
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            for (int i = 0; i < 20; ++i)
            {
                co_await partition->Put(std::format("key_{}", i), std::vector<char>(200, 'A'));
            }

            // Update to create fragmentation
            for (int i = 0; i < 10; ++i)
            {
                co_await partition->Put(std::format("key_{}", i), std::vector<char>(200, 'B'));
            }

            co_await partition->Compact();
            co_await partition->AsyncClose();
        }

        // Phase 2: Reopen and verify
        {
            auto open_res = co_await Partition::Open(config_, *worker_, 0);
            if (!open_res.has_value()) co_return;
            auto partition = std::move(open_res.value());

            for (int i = 0; i < 20; ++i)
            {
                auto result = co_await partition->Get(std::format("key_{}", i));
                EXPECT_TRUE(result.has_value());
                if (result.has_value()) {
                    EXPECT_TRUE(result.value().has_value());

                    if (result.value().has_value()) {
                        // First 10 should have 'B', rest should have 'A'
                        char expected = (i < 10) ? 'B' : 'A';
                        EXPECT_EQ(result.value().value()[0], expected);
                    }
                }
            }
        }
    };

    SyncWait(test());
}

// ============================================================================
// Concurrent Updates During Compaction
// ============================================================================

TEST_F(PartitionTest, ConcurrentUpdatesDuringCompaction)
{
    //
    // This tests the KeyDir CAS logic during compaction.
    //
    // If a key is updated while compaction is moving it, the compaction
    // should NOT overwrite the newer value in KeyDir.
    //

    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        // Seed data
        for (int i = 0; i < 100; ++i)
        {
            co_await partition->Put(std::format("static_{}", i), std::vector<char>(100, 'A'));
        }

        // Create garbage
        for (int i = 0; i < 50; ++i)
        {
            co_await partition->Put(std::format("static_{}", i), std::vector<char>(100, 'A'));
        }

        // Initial write of race key
        co_await partition->Put("race_key", std::vector<char>{'I', 'N', 'I', 'T'});

        // Run compaction and writer concurrently
        auto compactor = [&]() -> Task<void>
        {
            co_await SwitchToWorker(*worker_);
            for (int i = 0; i < 10; ++i)
            {
                co_await worker_->AsyncSleep(std::chrono::milliseconds(1));
            }
            co_await partition->Compact();
        };

        auto writer = [&]() -> Task<void>
        {
            co_await SwitchToWorker(*worker_);
            for (int i = 0; i < 50; ++i)
            {
                std::string val = std::format("V_{}", i);
                co_await partition->Put("race_key", std::vector<char>(val.begin(), val.end()));
                if (i % 10 == 0)
                {
                    co_await worker_->AsyncSleep(std::chrono::milliseconds(1));
                }
            }
            co_await partition->Put("race_key", std::vector<char>{'F', 'I', 'N', 'A', 'L'});
        };

        auto t1 = compactor();
        auto t2 = writer();

        co_await std::move(t1);
        co_await std::move(t2);

        // Verify final value
        auto result = co_await partition->Get("race_key");
        EXPECT_TRUE(result.has_value());
        if (result.has_value() && result.value().has_value()) {
            std::string actual(result.value().value().begin(), result.value().value().end());
            EXPECT_EQ(actual, "FINAL") << "Compaction overwrote concurrent update!";
        }

        co_await partition->AsyncClose();
    };

    SyncWait(test());
}

// ============================================================================
// Stats
// ============================================================================

TEST_F(PartitionTest, StatsAccuracy)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        co_await partition->Put("key1", {'v', '1'});
        co_await partition->Put("key2", {'v', '2'});
        co_await partition->Put("key1", {'u', 'p', 'd'});  // Update

        co_await partition->Get("key1");
        co_await partition->Get("key2");
        co_await partition->Get("missing");

        co_await partition->Del("key2");

        auto stats = partition->GetStats();

        EXPECT_EQ(stats.puts_total, 3);
        EXPECT_EQ(stats.gets_total, 3);
        EXPECT_EQ(stats.gets_miss_total, 1);
        EXPECT_EQ(stats.deletes_total, 1);
    };

    SyncWait(test());
}

TEST_F(PartitionTest, FragmentationCalculation)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto open_res = co_await Partition::Open(config_, *worker_, 0);
        if (!open_res.has_value()) co_return;
        auto partition = std::move(open_res.value());

        // Initial writes
        co_await partition->Put("key1", std::vector<char>(100, 'X'));
        co_await partition->Put("key2", std::vector<char>(100, 'Y'));
        co_await partition->Put("key3", std::vector<char>(100, 'Z'));

        // Create dead data
        co_await partition->Put("key1", std::vector<char>(100, 'A'));
        co_await partition->Del("key2");

        auto stats = partition->GetStats();
        double fragmentation = stats.OverallFragmentation();

        EXPECT_GT(fragmentation, 0.0) << "Should have fragmentation";
        EXPECT_LT(fragmentation, 1.0);
    };

    SyncWait(test());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}