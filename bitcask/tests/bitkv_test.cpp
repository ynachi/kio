//
// Created by Yao ACHI on 30/11/2025.
//

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

#include "kio/sync/sync_wait.h"
#include "bitcask/include/bitcask.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class BitKVTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;
    BitcaskConfig config_;
    WorkerConfig io_config_;

    void SetUp() override {
        // Configure logger (optional, disabled for cleaner test output)
        alog::configure(1024, LogLevel::Disabled);

        test_dir_ = std::filesystem::temp_directory_path() / "bitkv_integration_test";

        // Clean start
        if (std::filesystem::exists(test_dir_)) {
            std::filesystem::remove_all(test_dir_);
        }
        std::filesystem::create_directories(test_dir_);

        // Default Config
        config_.directory = test_dir_;
        config_.max_file_size = 1024 * 1024; // 1MB
        config_.read_buffer_size = 16 * 1024;
        config_.write_buffer_size = 16 * 1024;
        config_.sync_on_write = false;
        config_.auto_compact = false; // We test compaction manually mostly

        // IO Config
        io_config_.uring_queue_depth = 256;
    }

    void TearDown() override {
        if (std::filesystem::exists(test_dir_)) {
            std::filesystem::remove_all(test_dir_);
        }
    }

    // Helper to generate random value
    std::vector<char> random_blob(size_t size) {
        std::vector<char> v(size);
        for (size_t i = 0; i < size; ++i) {
            v[i] = static_cast<char>(rand() % 256);
        }
        return v;
    }
};

// ============================================================================
// 1. Basic Lifecycle Tests
// ============================================================================

TEST_F(BitKVTest, OpenAndClose) {
    // 1. Open DB
    auto open_res = SyncWait(BitKV::open(config_, io_config_, 2));
    // ASSERT is fine here because we are in the main test body, not a coroutine
    EXPECT_TRUE(open_res.has_value());
    auto db = std::move(open_res.value());

    // 2. Verify Directory Structure
    EXPECT_TRUE(std::filesystem::exists(test_dir_ / "partition_0"));
    EXPECT_TRUE(std::filesystem::exists(test_dir_ / "partition_1"));

    // 3. Close DB
    auto close_res = SyncWait(db->close());
    ASSERT_TRUE(close_res.has_value());

    // 4. Destructor runs here (safe on main thread)
}

// ============================================================================
// 2. CRUD Operations
// ============================================================================

TEST_F(BitKVTest, BasicPutGetDelete) {
    auto db = SyncWait(BitKV::open(config_, io_config_, 4)).value();

    auto test_task = [&](BitKV* db_ptr) -> Task<void> {
        // PUT
        std::string key = "user:1001";
        std::string value = "{\"name\": \"Alice\", \"age\": 30}";

        auto put_res = co_await db_ptr->put(key, value);
        EXPECT_TRUE(put_res.has_value());

        // GET
        auto get_res = co_await db_ptr->get_string(key);
        // Switched to EXPECT_* inside coroutine
        EXPECT_TRUE(get_res.has_value());
        if (get_res.has_value()) {
            EXPECT_TRUE(get_res.value().has_value());
            if (get_res.value().has_value()) {
                EXPECT_EQ(get_res.value().value(), value);
            }
        }

        // DELETE
        auto del_res = co_await db_ptr->del(key);
        EXPECT_TRUE(del_res.has_value());

        // GET (Miss)
        auto get_miss = co_await db_ptr->get_string(key);
        EXPECT_TRUE(get_miss.has_value());
        if (get_miss.has_value()) {
            EXPECT_FALSE(get_miss.value().has_value());
        }
    };

    SyncWait(test_task(db.get()));
    SyncWait(db->close());
}

TEST_F(BitKVTest, BinaryData) {
    auto db = SyncWait(BitKV::open(config_, io_config_, 2)).value();

    auto test_task = [&](BitKV* db_ptr) -> Task<void> {
        std::string key = "binary_image";
        std::vector<char> blob = random_blob(50 * 1024); // 50KB

        // Put binary
        auto put_res = co_await db_ptr->put(std::string(key), std::vector<char>(blob));
        EXPECT_TRUE(put_res.has_value());

        // Get binary
        auto get_res = co_await db_ptr->get(key);
        // Switched to EXPECT_* inside coroutine
        EXPECT_TRUE(get_res.has_value());
        if (get_res.has_value()) {
            EXPECT_TRUE(get_res.value().has_value());
            if (get_res.value().has_value()) {
                EXPECT_EQ(get_res.value().value(), blob);
            }
        }
    };

    SyncWait(test_task(db.get()));
    SyncWait(db->close());
}

// ============================================================================
// 3. Partition Routing & Volume
// ============================================================================

TEST_F(BitKVTest, MultiPartitionRouting) {
    // Use 4 partitions
    auto db = SyncWait(BitKV::open(config_, io_config_, 4)).value();

    auto test_task = [&](BitKV* db_ptr) -> Task<void> {
        // Write 1000 keys. Statistically, this should cover all 4 partitions.
        int count = 1000;
        for (int i = 0; i < count; ++i) {
            std::string key = std::format("key_{}", i);
            std::string val = std::format("val_{}", i);

            auto res = co_await db_ptr->put(std::move(key), std::move(val));
            EXPECT_TRUE(res.has_value());
        }

        // Verify all 1000
        for (int i = 0; i < count; ++i) {
            std::string key = std::format("key_{}", i);
            std::string expected = std::format("val_{}", i);

            auto res = co_await db_ptr->get_string(key);
            // Switched to EXPECT_* inside coroutine
            EXPECT_TRUE(res.has_value());
            if (res.has_value()) {
                EXPECT_TRUE(res.value().has_value());
                if (res.value().has_value()) {
                    EXPECT_EQ(res.value().value(), expected);
                }
            }
        }
    };

    SyncWait(test_task(db.get()));
    SyncWait(db->close());
}

// ============================================================================
// 4. Persistence (Recovery)
// ============================================================================

TEST_F(BitKVTest, DataSurvivesRestart) {
    // Phase 1: Write data
    {
        auto db = SyncWait(BitKV::open(config_, io_config_, 2)).value();
        auto task = [&](BitKV* ptr) -> Task<void> {
            co_await ptr->put("persistent_key", "persistent_value");
            co_await ptr->put("temp_key", "temp_value");
            co_await ptr->del("temp_key"); // Should effectively remove it
        };
        SyncWait(task(db.get()));
        SyncWait(db->close());
        // db destroyed
    }

    // Phase 2: Reopen and Verify
    {
        auto db = SyncWait(BitKV::open(config_, io_config_, 2)).value();
        auto task = [&](BitKV* ptr) -> Task<void> {
            // Check existing key
            auto res1 = co_await ptr->get_string("persistent_key");
            // Switched to EXPECT_* inside coroutine
            EXPECT_TRUE(res1.has_value());
            if (res1.has_value()) {
                EXPECT_TRUE(res1.value().has_value());
                if (res1.value().has_value()) {
                    EXPECT_EQ(res1.value().value(), "persistent_value");
                }
            }

            // Check deleted key
            auto res2 = co_await ptr->get_string("temp_key");
            EXPECT_TRUE(res2.has_value());
            if (res2.has_value()) {
                EXPECT_FALSE(res2.value().has_value());
            }
        };
        SyncWait(task(db.get()));
        SyncWait(db->close());
    }
}

// ============================================================================
// 5. Compaction Integration
// ============================================================================

TEST_F(BitKVTest, ManualCompaction) {
    auto db = SyncWait(BitKV::open(config_, io_config_, 2)).value();

    auto task = [&](BitKV* ptr) -> Task<void> {
        // Create fragmentation
        std::string key = "frag_key";
        std::string val(1024, 'A'); // 1KB

        // Write same key 100 times
        for(int i=0; i<100; ++i) {
            co_await ptr->put(std::string(key), std::vector<char>(val.begin(), val.end()));
        }

        // Trigger compaction on all partitions
        auto compact_res = co_await ptr->compact();
        EXPECT_TRUE(compact_res.has_value());

        // Verify data is still correct
        auto get_res = co_await ptr->get_string(key);
        // Switched to EXPECT_* inside coroutine
        EXPECT_TRUE(get_res.has_value());
        if (get_res.has_value()) {
            EXPECT_TRUE(get_res.value().has_value());
            if (get_res.value().has_value()) {
                EXPECT_EQ(get_res.value().value(), val);
            }
        }
    };

    SyncWait(task(db.get()));
    SyncWait(db->close());
}

// ============================================================================
// 6. Concurrency Stress Test
// ============================================================================

TEST_F(BitKVTest, ConcurrentAccess) {
    auto db = SyncWait(BitKV::open(config_, io_config_, 4)).value();

    // We will launch multiple concurrent coroutines operating on the DB.
    // Since BitKV uses an internal IOPool, these requests will be distributed
    // across the worker threads.

    int num_tasks = 10;
    int ops_per_task = 200;

    auto worker_task = [&](BitKV* ptr, int task_id) -> Task<void> {
        // Random seed per task
        std::mt19937 rng(task_id);

        for(int i=0; i<ops_per_task; ++i) {
            std::string key = std::format("task_{}_key_{}", task_id, i);
            std::string val = std::format("value_{}", i);

            // PUT
            auto res = co_await ptr->put(key, val);
            EXPECT_TRUE(res.has_value());

            // GET immediately
            auto get_res = co_await ptr->get_string(key);
            // Switched to EXPECT_* inside coroutine
            EXPECT_TRUE(get_res.has_value());
            if (get_res.has_value()) {
                EXPECT_TRUE(get_res.value().has_value());
                if(get_res.value().has_value()) {
                    EXPECT_EQ(get_res.value().value(), val);
                }
            }
        }
    };

    // Launch all tasks
    std::vector<Task<void>> tasks;
    tasks.reserve(num_tasks);
    for(int i=0; i<num_tasks; ++i) {
        tasks.push_back(worker_task(db.get(), i));
    }

    // Wait for all
    for(auto& t : tasks) {
        SyncWait(std::move(t));
    }

    SyncWait(db->close());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}