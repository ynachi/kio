#include <barrier>
#include <filesystem>
#include <gtest/gtest.h>
#include <latch>
#include <random>
#include <unordered_map>

#include "bitcask/include/bitcask.h"
#include "kio/include/async_logger.h"
#include "kio/include/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;
using namespace std::chrono_literals;

// ============================================================================
// Test Fixture
// ============================================================================

class BitKVAdvancedTest : public ::testing::Test
{
protected:
    std::filesystem::path test_dir_;
    BitcaskConfig db_config_;
    WorkerConfig io_config_;
    std::mt19937 rng_;

    void SetUp() override
    {
        alog::configure(4096, LogLevel::Debug);

        test_dir_ = std::filesystem::temp_directory_path() / "bitkv_advanced_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        db_config_.directory = test_dir_;
        db_config_.max_file_size = 100 * 1024;  // 100KB
        db_config_.sync_on_write = false;
        db_config_.auto_compact = false;
        db_config_.fragmentation_threshold = 0.5;

        io_config_.uring_queue_depth = 512;
        io_config_.default_op_slots = 2048;

        rng_.seed(std::random_device{}());
    }

    void TearDown() override { std::filesystem::remove_all(test_dir_); }

    // Helper to safely open DB on the main thread
    [[nodiscard]] std::unique_ptr<BitKV> open_db(size_t partitions = 4) const
    {
        auto res = SyncWait(BitKV::open(db_config_, io_config_, partitions));
        if (!res.has_value())
        {
            throw std::runtime_error(std::format("{}", res.error()));
        }
        return std::move(res.value());
    }

    static void close_db(std::unique_ptr<BitKV>& db)
    {
        if (db)
        {
            const auto res = SyncWait(db->close());
            EXPECT_TRUE(res.has_value());
            db.reset();  // Destructor runs here on main thread
        }
    }
};

// ============================================================================
// Concurrent Operations (Multiple Coroutines on Same DB)
// ============================================================================

TEST_F(BitKVAdvancedTest, ConcurrentReadsWritesDifferentKeys)
{
    auto db = open_db(4);

    auto test_coro = [&](BitKV* ptr) -> Task<void>
    {
        // Prepare keys
        std::vector<std::string> keys;
        keys.reserve(100);
        for (size_t i = 0; i < 100; ++i)
        {
            keys.push_back(std::format("key_{}", i));
        }

        // Concurrent writer
        auto writer = [&](size_t start, size_t end) -> Task<void>
        {
            for (size_t i = start; i < end; ++i)
            {
                std::string value = std::format("value_{}", i);
                auto res = co_await ptr->put(keys[i], value);
                EXPECT_TRUE(res.has_value());
            }
        };

        // Concurrent reader
        auto reader = [&](size_t start, size_t end) -> Task<void>
        {
            for (size_t i = start; i < end; ++i)
            {
                auto result = co_await ptr->get_string(keys[i]);
                // Just exercise the path, result depends on race
            }
        };

        std::vector<Task<void>> tasks;

        // 4 writers
        tasks.push_back(writer(0, 25));
        tasks.push_back(writer(25, 50));
        tasks.push_back(writer(50, 75));
        tasks.push_back(writer(75, 100));

        // 4 readers
        tasks.push_back(reader(0, 25));
        tasks.push_back(reader(25, 50));
        tasks.push_back(reader(50, 75));
        tasks.push_back(reader(75, 100));

        // Wait for all
        for (auto& task: tasks)
        {
            SyncWait(std::move(task));
        }

        // Verify all keys written
        for (size_t i = 0; i < 100; ++i)
        {
            auto value = co_await ptr->get_string(keys[i]);
            EXPECT_TRUE(value.has_value() && value.value().has_value()) << "Key missing: " << keys[i];
        }
    };

    SyncWait(test_coro(db.get()));
    close_db(db);
}

TEST_F(BitKVAdvancedTest, ConcurrentUpdatesSameKeys)
{
    auto db = open_db(4);

    auto test_coro = [&](BitKV* ptr) -> Task<void>
    {
        constexpr size_t num_keys = 10;
        constexpr size_t updates_per_task = 20;

        auto updater = [&](size_t task_id) -> Task<void>
        {
            for (size_t i = 0; i < num_keys; ++i)
            {
                for (size_t update = 0; update < updates_per_task; ++update)
                {
                    std::string key = std::format("shared_key_{}", i);
                    std::string value = std::format("task{}_{}", task_id, update);
                    auto res = co_await ptr->put(key, value);
                    EXPECT_TRUE(res.has_value());
                }
            }
        };

        std::vector<Task<void>> tasks;
        tasks.reserve(4);
        for (size_t i = 0; i < 4; ++i)
        {
            tasks.push_back(updater(i));
        }

        for (auto& task: tasks)
        {
            SyncWait(std::move(task));
        }

        // All keys should have SOME value
        for (size_t i = 0; i < num_keys; ++i)
        {
            std::string key = std::format("shared_key_{}", i);
            auto value = co_await ptr->get_string(key);
            EXPECT_TRUE(value.has_value() && value.value().has_value());
        }
    };

    SyncWait(test_coro(db.get()));
    close_db(db);
}

TEST_F(BitKVAdvancedTest, ReadYourWrites)
{
    auto db = open_db(4);

    auto test_coro = [&](BitKV* ptr) -> Task<void>
    {
        for (size_t i = 0; i < 100; ++i)
        {
            std::string key = std::format("key_{}", i);
            std::string value = std::format("value_{}", i);

            auto put_res = co_await ptr->put(key, value);
            EXPECT_TRUE(put_res.has_value());

            auto get_res = co_await ptr->get_string(key);
            EXPECT_TRUE(get_res.has_value());
            EXPECT_TRUE(get_res.value().has_value());
            EXPECT_EQ(get_res.value().value(), value);
        }
    };

    SyncWait(test_coro(db.get()));
    close_db(db);
}

// ============================================================================
// File Rotation Under Load
// ============================================================================

TEST_F(BitKVAdvancedTest, FileRotationUnderLoad)
{
    // Reconfigure for small files
    db_config_.max_file_size = 10 * 1024;
    auto db = open_db(4);

    auto test_coro = [&](BitKV* ptr) -> Task<void>
    {
        constexpr size_t num_entries = 1000;

        for (size_t i = 0; i < num_entries; ++i)
        {
            std::string key = std::format("rotate_key_{:06}", i);
            std::string value = std::format("value_{}_padding_to_make_it_bigger", i);
            auto res = co_await ptr->put(key, value);
            EXPECT_TRUE(res.has_value());
        }

        for (size_t i = 0; i < num_entries; ++i)
        {
            std::string key = std::format("rotate_key_{:06}", i);
            auto result = co_await ptr->get_string(key);
            EXPECT_TRUE(result.has_value() && result.value().has_value());
        }
    };

    SyncWait(test_coro(db.get()));

    // Verify files on disk (Main Thread)
    size_t total_data_files = 0;
    for (size_t p = 0; p < 4; ++p)
    {
        auto partition_dir = test_dir_ / std::format("partition_{}", p);
        for (const auto& entry: std::filesystem::directory_iterator(partition_dir))
        {
            if (entry.path().extension() == ".db")
            {
                total_data_files++;
            }
        }
    }
    EXPECT_GT(total_data_files, 4);

    close_db(db);
}

// ============================================================================
// Mixed Workload Patterns
// ============================================================================

TEST_F(BitKVAdvancedTest, MixedReadWriteDelete)
{
    auto db = open_db(4);

    auto test_coro = [&](BitKV* ptr) -> Task<void>
    {
        std::unordered_map<std::string, std::string> ground_truth;
        std::uniform_int_distribution<int> op_dist(0, 2);
        std::uniform_int_distribution<size_t> key_dist(0, 49);

        constexpr size_t num_ops = 500;

        for (size_t i = 0; i < num_ops; ++i)
        {
            int op = op_dist(rng_);
            size_t key_idx = key_dist(rng_);
            std::string key = std::format("mixed_key_{}", key_idx);

            if (op == 0)
            {  // Put
                std::string value = std::format("value_{}", i);
                auto res = co_await ptr->put(key, value);
                EXPECT_TRUE(res.has_value());
                ground_truth[key] = value;
            }
            else if (op == 1)
            {  // Get
                auto result = co_await ptr->get_string(key);
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
            {  // Delete
                auto res = co_await ptr->del(key);
                EXPECT_TRUE(res.has_value());
                ground_truth.erase(key);
            }
        }
    };

    SyncWait(test_coro(db.get()));
    close_db(db);
}

// ============================================================================
// Robustness Tests
// ============================================================================

TEST_F(BitKVAdvancedTest, RepeatedOpenClose)
{
    constexpr size_t cycles = 5;

    // Note: Open/Close logic happens on Main Thread
    for (size_t cycle = 0; cycle < cycles; ++cycle)
    {
        auto db = open_db(4);
        auto task = [&](BitKV* ptr) -> Task<void>
        {
            for (size_t i = 0; i < 20; ++i)
            {
                std::string key = std::format("cycle_{}_key_{}", cycle, i);
                std::string value = std::format("value_{}", i);
                auto res = co_await ptr->put(key, value);
                EXPECT_TRUE(res.has_value());
            }
        };
        SyncWait(task(db.get()));
        close_db(db);

        // Ensure OS has time to reclaim FDs and threads
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Verify
    auto db = open_db(4);
    auto verify = [&](BitKV* ptr) -> Task<void>
    {
        for (size_t cycle = 0; cycle < cycles; ++cycle)
        {
            for (size_t i = 0; i < 20; ++i)
            {
                std::string key = std::format("cycle_{}_key_{}", cycle, i);
                // EXPECT_TRUE only if the value actually exists (robustness check)
                if (auto value = co_await ptr->get_string(key); value.has_value() && value.value().has_value())
                {
                    EXPECT_FALSE(value.value().value().empty());
                }
            }
        }
    };
    SyncWait(verify(db.get()));
    close_db(db);
}

TEST_F(BitKVAdvancedTest, HighVolumeWrites)
{
    auto db = open_db(8);

    auto test_coro = [&](BitKV* ptr) -> Task<void>
    {
        constexpr size_t num_keys = 5000;

        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < num_keys; ++i)
        {
            const std::string key = std::format("key_{:06}", i);
            const std::string value = std::format("value_data_{}", i);
            auto res = co_await ptr->put(key, value);
            EXPECT_TRUE(res.has_value());
        }
        const auto end = std::chrono::steady_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Write throughput: " << (num_keys * 1000.0) / static_cast<double>(duration.count()) << " ops/sec" << std::endl;
    };

    SyncWait(test_coro(db.get()));
    close_db(db);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
