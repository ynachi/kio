//
// Integration tests for BitKV (Full Database)
//
// Tests the complete Bitcask database:
// - Multi-partition routing
// - Concurrent access patterns
// - Persistence across restarts
// - Compaction integration
//

#include <gtest/gtest.h>
#include <filesystem>
#include <random>

#include "bitcask/include/bitcask.h"
#include "kio/sync/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class BitKVTest : public ::testing::Test
{
protected:
    std::filesystem::path test_dir_;
    BitcaskConfig config_;
    WorkerConfig io_config_;

    void SetUp() override
    {
        alog::Configure(1024, LogLevel::kDisabled);

        test_dir_ = std::filesystem::temp_directory_path() / "bitkv_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        config_.directory = test_dir_;
        config_.max_file_size = 100 * 1024;  // 100KB
        config_.sync_on_write = false;
        config_.auto_compact = false;

        io_config_.uring_queue_depth = 256;
    }

    void TearDown() override
    {
        std::filesystem::remove_all(test_dir_);
    }

    // Helper to open DB synchronously
    std::unique_ptr<BitKV> OpenDb(size_t partitions = 4)
    {
        auto result = SyncWait(BitKV::Open(config_, io_config_, partitions));
        if (!result.has_value())
        {
            throw std::runtime_error(std::format("Failed to open DB: {}", result.error()));
        }
        return std::move(result.value());
    }

    void CloseDb(std::unique_ptr<BitKV>& db)
    {
        if (db)
        {
            auto result = SyncWait(db->Close());
            EXPECT_TRUE(result.has_value());
            db.reset();
        }
    }
};

// ============================================================================
// Lifecycle
// ============================================================================

TEST_F(BitKVTest, OpenAndClose)
{
    auto db = OpenDb(2);

    // Verify partition directories created
    EXPECT_TRUE(std::filesystem::exists(test_dir_ / "partition_0"));
    EXPECT_TRUE(std::filesystem::exists(test_dir_ / "partition_1"));

    CloseDb(db);
}

// ============================================================================
// Basic CRUD
// ============================================================================

TEST_F(BitKVTest, PutGetDelete)
{
    auto db = OpenDb(4);

    auto test = [&](BitKV* ptr) -> Task<void>
    {
        std::string const key = "user:1001";
        std::string value = R"({"name": "Alice", "age": 30})";

        // Put
        auto put_result = co_await ptr->Put(key, value);
        EXPECT_TRUE(put_result.has_value());

        // Get
        auto get_result = co_await ptr->GetString(key);
        EXPECT_TRUE(get_result.has_value());
        EXPECT_TRUE(get_result.value().has_value());
        EXPECT_EQ(get_result.value().value(), value);

        // Delete
        auto del_result = co_await ptr->Del(key);
        EXPECT_TRUE(del_result.has_value());

        // Get (miss)
        auto get_miss = co_await ptr->GetString(key);
        EXPECT_TRUE(get_miss.has_value());
        EXPECT_FALSE(get_miss.value().has_value());
    };

    SyncWait(test(db.get()));
    CloseDb(db);
}

TEST_F(BitKVTest, BinaryData)
{
    auto db = OpenDb(2);

    auto test = [&](BitKV* ptr) -> Task<void>
    {
        std::string key = "binary_blob";
        std::vector<char> blob(50 * 1024);  // 50KB
        for (size_t i = 0; i < blob.size(); ++i)
        {
            blob[i] = static_cast<char>(i % 256);
        }

        auto put_result = co_await ptr->Put(std::string(key), std::vector<char>(blob));
        EXPECT_TRUE(put_result.has_value());

        auto get_result = co_await ptr->Get(key);
        EXPECT_TRUE(get_result.has_value());
        EXPECT_TRUE(get_result.value().has_value());
        EXPECT_EQ(get_result.value().value(), blob);
    };

    SyncWait(test(db.get()));
    CloseDb(db);
}

// ============================================================================
// Multi-Partition Routing
// ============================================================================

TEST_F(BitKVTest, KeysDistributedAcrossPartitions)
{
    auto db = OpenDb(4);

    auto test = [&](BitKV* ptr) -> Task<void>
    {
        // Write 1000 keys - should distribute across partitions
        for (int i = 0; i < 1000; ++i)
        {
            auto result = co_await ptr->Put(
                std::format("key_{}", i),
                std::format("value_{}", i)
            );
            EXPECT_TRUE(result.has_value());
        }

        // Verify all readable
        for (int i = 0; i < 1000; ++i)
        {
            auto result = co_await ptr->GetString(std::format("key_{}", i));
            EXPECT_TRUE(result.has_value());
            EXPECT_TRUE(result.value().has_value());
            EXPECT_EQ(result.value().value(), std::format("value_{}", i));
        }
    };

    SyncWait(test(db.get()));

    // Verify multiple partitions have data files
    int partitions_with_data = 0;
    for (int p = 0; p < 4; ++p)
    {
        auto partition_dir = test_dir_ / std::format("partition_{}", p);
        for (const auto& entry : std::filesystem::directory_iterator(partition_dir))
        {
            if (entry.path().extension() == ".db")
            {
                partitions_with_data++;
                break;
            }
        }
    }
    EXPECT_GE(partitions_with_data, 2) << "Keys should be distributed";

    CloseDb(db);
}

// ============================================================================
// Persistence
// ============================================================================

TEST_F(BitKVTest, DataSurvivesRestart)
{
    // Phase 1: Write data
    {
        auto db = OpenDb(2);

        auto task = [&](BitKV* ptr) -> Task<void>
        {
            co_await ptr->Put("persistent_key", "persistent_value");
            co_await ptr->Put("temp_key", "temp_value");
            co_await ptr->Del("temp_key");
        };

        SyncWait(task(db.get()));
        CloseDb(db);
    }

    // Phase 2: Reopen and verify
    {
        auto db = OpenDb(2);

        auto task = [&](BitKV* ptr) -> Task<void>
        {
            auto result1 = co_await ptr->GetString("persistent_key");
            EXPECT_TRUE(result1.value().has_value());
            EXPECT_EQ(result1.value().value(), "persistent_value");

            auto result2 = co_await ptr->GetString("temp_key");
            EXPECT_FALSE(result2.value().has_value());
        };

        SyncWait(task(db.get()));
        CloseDb(db);
    }
}

// ============================================================================
// Compaction
// ============================================================================

TEST_F(BitKVTest, ManualCompaction)
{
    auto db = OpenDb(2);

    auto task = [&](BitKV* ptr) -> Task<void>
    {
        std::string key = "frag_key";
        std::string val(1024, 'A');

        // Write same key 100 times (creates fragmentation)
        for (int i = 0; i < 100; ++i)
        {
            co_await ptr->Put(std::string(key), std::vector<char>(val.begin(), val.end()));
        }

        // Trigger compaction
        auto compact_result = co_await ptr->Compact();
        EXPECT_TRUE(compact_result.has_value());

        // Verify data intact
        auto get_result = co_await ptr->GetString(key);
        EXPECT_TRUE(get_result.value().has_value());
        EXPECT_EQ(get_result.value().value(), val);
    };

    SyncWait(task(db.get()));
    CloseDb(db);
}

// ============================================================================
// Concurrent Access
// ============================================================================

TEST_F(BitKVTest, ConcurrentReadWriteDifferentKeys)
{
    auto db = OpenDb(4);

    auto test = [&](BitKV* ptr) -> Task<void>
    {
        constexpr int num_keys = 100;

        // Generate keys
        std::vector<std::string> keys;
        for (int i = 0; i < num_keys; ++i)
        {
            keys.push_back(std::format("key_{}", i));
        }

        // Writer tasks
        auto writer = [&](int start, int end) -> Task<void>
        {
            for (int i = start; i < end; ++i)
            {
                auto result = co_await ptr->Put(keys[i], std::format("value_{}", i));
                EXPECT_TRUE(result.has_value());
            }
        };

        // Reader tasks
        auto reader = [&](int start, int end) -> Task<void>
        {
            for (int i = start; i < end; ++i)
            {
                auto result = co_await ptr->GetString(keys[i]);
                // May or may not exist depending on race
            }
        };

        // Run concurrently
        std::vector<Task<void>> tasks;
        tasks.push_back(writer(0, 25));
        tasks.push_back(writer(25, 50));
        tasks.push_back(writer(50, 75));
        tasks.push_back(writer(75, 100));
        tasks.push_back(reader(0, 25));
        tasks.push_back(reader(25, 50));
        tasks.push_back(reader(50, 75));
        tasks.push_back(reader(75, 100));

        for (auto& task : tasks)
        {
            SyncWait(std::move(task));
        }

        // Verify all keys written
        for (int i = 0; i < num_keys; ++i)
        {
            auto result = co_await ptr->GetString(keys[i]);
            EXPECT_TRUE(result.value().has_value()) << "Missing key: " << keys[i];
        }
    };

    SyncWait(test(db.get()));
    CloseDb(db);
}

TEST_F(BitKVTest, ConcurrentUpdatesSameKey)
{
    auto db = OpenDb(4);

    auto test = [&](BitKV* ptr) -> Task<void>
    {
        constexpr int num_updates = 20;

        auto updater = [&](int task_id) -> Task<void>
        {
            for (int i = 0; i < num_updates; ++i)
            {
                auto result = co_await ptr->Put(
                    "shared_key",
                    std::format("task{}_{}", task_id, i)
                );
                EXPECT_TRUE(result.has_value());
            }
        };

        // 4 tasks updating same key
        std::vector<Task<void>> tasks;
        for (int i = 0; i < 4; ++i)
        {
            tasks.push_back(updater(i));
        }

        for (auto& task : tasks)
        {
            SyncWait(std::move(task));
        }

        // Key should have SOME value
        auto result = co_await ptr->GetString("shared_key");
        EXPECT_TRUE(result.value().has_value());
    };

    SyncWait(test(db.get()));
    CloseDb(db);
}

TEST_F(BitKVTest, ReadYourWrites)
{
    auto db = OpenDb(4);

    auto test = [&](BitKV* ptr) -> Task<void>
    {
        for (int i = 0; i < 100; ++i)
        {
            std::string key = std::format("key_{}", i);
            std::string value = std::format("value_{}", i);

            auto put_result = co_await ptr->Put(key, value);
            EXPECT_TRUE(put_result.has_value());

            auto get_result = co_await ptr->GetString(key);
            EXPECT_TRUE(get_result.has_value());
            EXPECT_TRUE(get_result.value().has_value());
            EXPECT_EQ(get_result.value().value(), value);
        }
    };

    SyncWait(test(db.get()));
    CloseDb(db);
}

// ============================================================================
// Mixed Workload
// ============================================================================

TEST_F(BitKVTest, MixedOperations)
{
    auto db = OpenDb(4);
    std::mt19937 rng(42);

    auto test = [&](BitKV* ptr) -> Task<void>
    {
        std::unordered_map<std::string, std::string> ground_truth;
        std::uniform_int_distribution<int> op_dist(0, 2);
        std::uniform_int_distribution<int> key_dist(0, 49);

        for (int i = 0; i < 500; ++i)
        {
            int op = op_dist(rng);
            std::string key = std::format("mixed_key_{}", key_dist(rng));

            if (op == 0)
            {
                // Put
                std::string value = std::format("value_{}", i);
                auto result = co_await ptr->Put(key, value);
                EXPECT_TRUE(result.has_value());
                ground_truth[key] = value;
            }
            else if (op == 1)
            {
                // Get
                auto result = co_await ptr->GetString(key);
                EXPECT_TRUE(result.has_value());

                if (ground_truth.contains(key))
                {
                    EXPECT_TRUE(result.value().has_value());
                    if (result.value().has_value())
                    {
                        EXPECT_EQ(result.value().value(), ground_truth[key]);
                    }
                }
                else
                {
                    EXPECT_FALSE(result.value().has_value());
                }
            }
            else
            {
                // Delete
                auto result = co_await ptr->Del(key);
                EXPECT_TRUE(result.has_value());
                ground_truth.erase(key);
            }
        }
    };

    SyncWait(test(db.get()));
    CloseDb(db);
}

// ============================================================================
// Robustness
// ============================================================================

TEST_F(BitKVTest, RepeatedOpenClose)
{
    for (int cycle = 0; cycle < 5; ++cycle)
    {
        auto db = OpenDb(4);

        auto task = [&](BitKV* ptr) -> Task<void>
        {
            for (int i = 0; i < 20; ++i)
            {
                co_await ptr->Put(
                    std::format("cycle_{}_key_{}", cycle, i),
                    std::format("value_{}", i)
                );
            }
        };

        SyncWait(task(db.get()));
        CloseDb(db);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Verify all data
    auto db = OpenDb(4);

    auto verify = [&](BitKV* ptr) -> Task<void>
    {
        for (int cycle = 0; cycle < 5; ++cycle)
        {
            for (int i = 0; i < 20; ++i)
            {
                auto result = co_await ptr->GetString(std::format("cycle_{}_key_{}", cycle, i));
                EXPECT_TRUE(result.value().has_value())
                    << "Missing key from cycle " << cycle;
            }
        }
    };

    SyncWait(verify(db.get()));
    CloseDb(db);
}

TEST_F(BitKVTest, FileRotationUnderLoad)
{
    config_.max_file_size = 10 * 1024;  // 10KB
    auto db = OpenDb(4);

    auto test = [&](BitKV* ptr) -> Task<void>
    {
        for (int i = 0; i < 1000; ++i)
        {
            std::string value = std::format("value_{}_padding_data", i);
            auto result = co_await ptr->Put(std::format("rotate_key_{:06}", i), value);
            EXPECT_TRUE(result.has_value());
        }

        // Verify all readable
        for (int i = 0; i < 1000; ++i)
        {
            auto result = co_await ptr->GetString(std::format("rotate_key_{:06}", i));
            EXPECT_TRUE(result.value().has_value());
        }
    };

    SyncWait(test(db.get()));

    // Verify multiple files created
    int total_files = 0;
    for (int p = 0; p < 4; ++p)
    {
        auto partition_dir = test_dir_ / std::format("partition_{}", p);
        for (const auto& entry : std::filesystem::directory_iterator(partition_dir))
        {
            if (entry.path().extension() == ".db")
            {
                total_files++;
            }
        }
    }
    EXPECT_GT(total_files, 4) << "Should have rotated files";

    CloseDb(db);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}