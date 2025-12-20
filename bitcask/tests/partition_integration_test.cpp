//
// Created by Yao ACHI on 26/11/2025.
//

#include <filesystem>
#include <gtest/gtest.h>
#include <random>
#include <unordered_set>

#include "bitcask/include/partition.h"
#include "kio/core/worker.h"
#include "kio/sync/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class IntegrationTest : public ::testing::Test
{
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;
    BitcaskConfig config_;
    std::mt19937 rng_;

    void SetUp() override
    {
        alog::configure(1024, LogLevel::Disabled);
        test_dir_ = std::filesystem::temp_directory_path() / "bitcask_integration_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
        // create the partition dir too
        std::filesystem::create_directories(test_dir_ / "partition_0");

        config_.directory = test_dir_;
        config_.max_file_size = 50 * 1024;  // 50KB - realistic for testing
        config_.sync_on_write = false;
        config_.auto_compact = false;  // Manual control
        config_.fragmentation_threshold = 0.6;  // 60% threshold
        config_.read_buffer_size = 8 * 1024;
        config_.write_buffer_size = 4 * 1024;

        WorkerConfig worker_config;
        worker_config.uring_queue_depth = 256;
        worker_ = std::make_unique<Worker>(0, worker_config);

        worker_thread_ = std::thread([this]() { worker_->LoopForever(); });

        worker_->WaitReady();

        // Seed RNG
        rng_.seed(std::random_device{}());
    }

    void TearDown() override
    {
        if (worker_)
        {
            (void) worker_->RequestStop();
            worker_->WaitShutdown();
        }

        if (worker_thread_.joinable())
        {
            worker_thread_.join();
        }

        worker_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    // Helper: Generate random string
    std::string random_string(const size_t length)
    {
        constexpr char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);

        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i)
        {
            result += charset[dist(rng_)];
        }
        return result;
    }

    std::vector<char> random_value(const size_t min_size, const size_t max_size)
    {
        std::uniform_int_distribution size_dist(min_size, max_size);
        const size_t size = size_dist(rng_);

        std::vector<char> value(size);
        std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
        for (auto& byte: value)
        {
            byte = static_cast<char>(byte_dist(rng_));
        }
        return value;
    }
};

// ============================================================================
// Scenario 1: E-commerce Session Store
// ============================================================================

TEST_F(IntegrationTest, EcommerceSessionStore)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Simulate 1000 user sessions
        constexpr int num_sessions = 1000;
        std::vector<std::string> session_ids;

        // Phase 1: Create sessions
        for (int i = 0; i < num_sessions; ++i)
        {
            std::string session_id = std::format("session_{}", i);
            session_ids.push_back(session_id);

            // Session data: user_id, cart, preferences, etc.
            std::string session_data = std::format(R"({{"user_id":{},"cart":["item1","item2"],"timestamp":{}}})", i,
                                                   GetCurrentTimestamp());
            std::vector value(session_data.begin(), session_data.end());

            auto result = co_await partition->put(std::move(session_id), std::move(value));
            EXPECT_TRUE(result.has_value());
        }

        // Phase 2: Simulate session activity (updates)
        std::uniform_int_distribution session_dist(0, num_sessions - 1);
        for (int activity = 0; activity < 500; ++activity)
        {
            int session_idx = session_dist(rng_);
            const std::string& session_id = session_ids[session_idx];

            // Update session with new activity
            std::string updated_data =
                    std::format(R"({{"user_id":{},"cart_updated":true,"activity":{},"timestamp":{}}})", session_idx,
                                activity, GetCurrentTimestamp());
            std::vector value(updated_data.begin(), updated_data.end());

            co_await partition->put(std::string(session_id), std::move(value));
        }

        // Phase 3: Expire old sessions (simulate TTL)
        for (int i = 0; i < num_sessions / 2; ++i)
        {
            co_await partition->del(session_ids[i]);
        }

        // Phase 4: Compact to reclaim space
        auto compact_result = co_await partition->compact();
        EXPECT_TRUE(compact_result.has_value());

        // Phase 5: Verify active sessions
        int active_sessions = 0;
        for (int i = num_sessions / 2; i < num_sessions; ++i)
        {
            if (auto get_result = co_await partition->get(session_ids[i]);
                get_result.has_value() && get_result.value().has_value())
            {
                active_sessions++;
            }
        }

        EXPECT_EQ(active_sessions, num_sessions / 2);

        // Phase 6: Crash simulation - restart and verify
        auto stats_before = partition->get_stats();
        partition.reset();

        partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Verify sessions survived restart
        for (int i = num_sessions / 2; i < num_sessions; ++i)
        {
            auto get_result = co_await partition->get(session_ids[i]);
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                    << "Session " << session_ids[i] << " should survive restart";
        }

        // Expired sessions should still be deleted
        for (int i = 0; i < num_sessions / 2; ++i)
        {
            auto get_result = co_await partition->get(session_ids[i]);
            EXPECT_TRUE(get_result.has_value());
            EXPECT_FALSE(get_result.value().has_value()) << "Session " << session_ids[i] << " should stay deleted";
        }
    };

    SyncWait(test_coro());
}

// ============================================================================
// Scenario 2: Time-Series Metrics Store
// ============================================================================

TEST_F(IntegrationTest, TimeSeriesMetrics)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Simulate metrics from 10 servers over time
        constexpr int num_servers = 10;
        constexpr int num_timestamps = 500;

        // Phase 1: Write time-series data
        for (int ts = 0; ts < num_timestamps; ++ts)
        {
            for (int server = 0; server < num_servers; ++server)
            {
                std::string key = std::format("metrics:server_{}:ts_{}", server, ts);

                // Metric data: CPU, memory, disk, network
                std::string metric = std::format(R"({{"cpu":{},"mem":{},"disk":{},"net":{}}})", 50 + (ts % 50),
                                                 60 + (server % 40), 70 + (ts % 30), 80 + (server % 20));
                std::vector<char> value(metric.begin(), metric.end());

                auto result = co_await partition->put(std::move(key), std::move(value));
                EXPECT_TRUE(result.has_value());
            }
        }

        auto stats_after_write = partition->get_stats();
        EXPECT_EQ(stats_after_write.puts_total, num_servers * num_timestamps);

        // Phase 2: Query recent metrics (read hot data)
        int successful_reads = 0;
        for (int server = 0; server < num_servers; ++server)
        {
            // Read last 10 timestamps for each server
            for (int ts = num_timestamps - 10; ts < num_timestamps; ++ts)
            {
                std::string key = std::format("metrics:server_{}:ts_{}", server, ts);

                if (auto get_result = co_await partition->get(key);
                    get_result.has_value() && get_result.value().has_value())
                {
                    successful_reads++;
                }
            }
        }

        EXPECT_EQ(successful_reads, num_servers * 10);

        // Phase 3: Age-out old metrics (delete old timestamps)
        int retention_window = 100;  // Keep last 100 timestamps
        for (int ts = 0; ts < num_timestamps - retention_window; ++ts)
        {
            for (int server = 0; server < num_servers; ++server)
            {
                std::string key = std::format("metrics:server_{}:ts_{}", server, ts);
                co_await partition->del(key);
            }
        }

        // Phase 4: Compact
        auto compact_result = co_await partition->compact();
        EXPECT_TRUE(compact_result.has_value());

        auto stats_after_compact = partition->get_stats();
        EXPECT_GT(stats_after_compact.bytes_reclaimed_total, 0);

        // Phase 5: Verify only recent data exists
        for (int server = 0; server < num_servers; ++server)
        {
            for (int ts = num_timestamps - retention_window; ts < num_timestamps; ++ts)
            {
                std::string key = std::format("metrics:server_{}:ts_{}", server, ts);
                auto get_result = co_await partition->get(key);
                EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                        << "Recent metric " << key << " should exist";
            }
        }
    };

    SyncWait(test_coro());
}

// ============================================================================
// Scenario 3: Cache with High Update Rate
// ============================================================================

TEST_F(IntegrationTest, HighUpdateRateCache)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Simulate cache with 100 hot keys
        constexpr int num_hot_keys = 100;
        constexpr int updates_per_key = 50;

        std::vector<std::string> hot_keys;
        hot_keys.reserve(num_hot_keys);
        for (int i = 0; i < num_hot_keys; ++i)
        {
            hot_keys.push_back(std::format("cache:hot_key_{}", i));
        }

        // Phase 1: Write initial cache entries
        for (const auto& key: hot_keys)
        {
            std::vector<char> value = random_value(100, 500);
            co_await partition->put(std::string(key), std::move(value));
        }

        // Phase 2: Simulate high update rate (cache invalidations)
        std::uniform_int_distribution key_dist(0, num_hot_keys - 1);

        for (int update = 0; update < updates_per_key * num_hot_keys; ++update)
        {
            int key_idx = key_dist(rng_);
            std::string key = hot_keys[key_idx];

            // New cache value
            std::vector<char> value = random_value(100, 500);
            co_await partition->put(std::move(key), std::move(value));

            // Occasionally compact
            if (update % 1000 == 0 && update > 0)
            {
                if (auto stats = partition->get_stats();
                    stats.overall_fragmentation() > config_.fragmentation_threshold)
                {
                    co_await partition->compact();
                }
            }
        }

        auto stats_final = partition->get_stats();

        // Should have significant fragmentation or compaction activity
        EXPECT_TRUE(stats_final.overall_fragmentation() > 0.3 || stats_final.compactions_total > 0);

        // Phase 3: Verify all hot keys are accessible with latest values
        for (const auto& key: hot_keys)
        {
            auto get_result = co_await partition->get(key);
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                    << "Hot key " << key << " should be accessible";
        }

        // Phase 4: Recovery test
        partition.reset();
        partition = (co_await Partition::open(config_, *worker_, 0)).value();

        for (const auto& key: hot_keys)
        {
            auto get_result = co_await partition->get(key);
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                    << "Hot key " << key << " should survive restart";
        }
    };

    SyncWait(test_coro());
}

// ============================================================================
// Scenario 4: Mixed Workload (CRUD Operations)
// ============================================================================

TEST_F(IntegrationTest, MixedCRUDWorkload)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        constexpr int num_operations = 2000;
        std::unordered_set<std::string> live_keys;
        std::unordered_map<std::string, std::vector<char>> expected_values;

        std::uniform_int_distribution op_dist(0, 99);
        int key_counter = 0;

        // Mixed workload: 40% create, 30% update, 20% read, 10% delete
        for (int op = 0; op < num_operations; ++op)
        {
            if (int op_type = op_dist(rng_); op_type < 40 || live_keys.empty())
            {
                // CREATE (40%)
                std::string key = std::format("key_{}", key_counter++);
                std::vector<char> value = random_value(50, 300);

                auto result = co_await partition->put(std::string(key), std::vector<char>(value));
                EXPECT_TRUE(result.has_value());

                live_keys.insert(key);
                expected_values[key] = value;
            }
            else if (op_type < 70)
            {
                // UPDATE (30%)
                auto it = live_keys.begin();
                std::advance(it, rng_() % live_keys.size());
                std::string key = *it;

                std::vector<char> new_value = random_value(50, 300);
                co_await partition->put(std::string(key), std::vector(new_value));

                expected_values[key] = new_value;
            }
            else if (op_type < 90)
            {
                // READ (20%)
                auto it = live_keys.begin();
                std::advance(it, rng_() % live_keys.size());
                std::string key = *it;

                auto get_result = co_await partition->get(key);
                EXPECT_TRUE(get_result.has_value() && get_result.value().has_value());

                if (get_result.has_value() && get_result.value().has_value())
                {
                    EXPECT_EQ(get_result.value().value(), expected_values[key]) << "Key " << key << " has wrong value";
                }
            }
            else
            {
                // DELETE (10%)
                auto it = live_keys.begin();
                std::advance(it, rng_() % live_keys.size());
                std::string key = *it;

                co_await partition->del(key);
                live_keys.erase(it);
                expected_values.erase(key);
            }

            // Periodic compaction
            if (op % 500 == 0 && op > 0)
            {
                if (auto stats = partition->get_stats();
                    stats.overall_fragmentation() > config_.fragmentation_threshold)
                {
                    co_await partition->compact();
                }
            }
        }

        // Final verification
        for (const auto& key: live_keys)
        {
            auto get_result = co_await partition->get(key);
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                    << "Live key " << key << " should exist";

            if (get_result.has_value() && get_result.value().has_value())
            {
                EXPECT_EQ(get_result.value().value(), expected_values[key]);
            }
        }

        auto stats = partition->get_stats();
        EXPECT_GT(stats.puts_total, 0);
        EXPECT_GT(stats.gets_total, 0);
        EXPECT_GT(stats.deletes_total, 0);
    };

    SyncWait(test_coro());
}

// ============================================================================
// Scenario 5: Large Value Storage (Document Store)
// ============================================================================

TEST_F(IntegrationTest, LargeDocumentStore)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        const auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        // Simulate document store with varying document sizes
        constexpr int num_documents = 100;
        std::vector<std::string> doc_ids;

        // Phase 1: Store documents
        for (int i = 0; i < num_documents; ++i)
        {
            std::string doc_id = std::format("doc_{}", i);
            doc_ids.push_back(doc_id);

            std::vector<char> document = random_value(1024, 10 * 1024);

            auto result = co_await partition->put(std::move(doc_id), std::move(document));
            EXPECT_TRUE(result.has_value());
        }

        auto stats_after_write = partition->get_stats();

        // Should have multiple files due to large values (100 docs * avg 5KB = 500KB > 50KB max file)
        EXPECT_GE(stats_after_write.data_files.size(), 2);
        EXPECT_GE(stats_after_write.file_rotations_total, 1);

        // Phase 2: Update some documents (simulate edits)
        for (int i = 0; i < num_documents / 3; ++i)
        {
            std::string doc_id = doc_ids[i];
            std::vector<char> updated_doc = random_value(2 * 1024, 15 * 1024);

            co_await partition->put(std::move(doc_id), std::move(updated_doc));
        }

        // Phase 3: Delete some documents
        for (int i = num_documents * 2 / 3; i < num_documents; ++i)
        {
            co_await partition->del(doc_ids[i]);
        }

        // Phase 4: Compact
        auto compact_result = co_await partition->compact();
        EXPECT_TRUE(compact_result.has_value());

        auto stats_after_compact = partition->get_stats();
        // Compaction should happen because updating 33% + deleting 33% = 66% dead data in old files
        EXPECT_GT(stats_after_compact.bytes_reclaimed_total, 0);

        // Phase 5: Verify documents
        for (int i = 0; i < num_documents * 2 / 3; ++i)
        {
            auto get_result = co_await partition->get(doc_ids[i]);
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                    << "Document " << doc_ids[i] << " should exist";
        }

        // Deleted documents should not exist
        for (int i = num_documents * 2 / 3; i < num_documents; ++i)
        {
            auto get_result = co_await partition->get(doc_ids[i]);
            EXPECT_TRUE(get_result.has_value());
            EXPECT_FALSE(get_result.value().has_value());
        }

        // Phase 6: Recovery test with large values
        co_await partition->async_close();

        auto recovery_res = co_await Partition::open(config_, *worker_, 0);
        EXPECT_TRUE(recovery_res.has_value());
        auto partition_rec = std::move(recovery_res.value());

        for (int i = 0; i < num_documents * 2 / 3; ++i)
        {
            auto get_result = co_await partition_rec->get(doc_ids[i]);
            EXPECT_TRUE(get_result.has_value() && get_result.value().has_value())
                    << "Document " << doc_ids[i] << " should survive restart";
        }

        co_await partition_rec->async_close();
    };

    SyncWait(test_coro());
}

// ============================================================================
// Scenario 6: Stress Test - Rapid Fire Operations
// ============================================================================

TEST_F(IntegrationTest, RapidFireOperations)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto partition = (co_await Partition::open(config_, *worker_, 0)).value();

        constexpr int num_rapid_ops = 5000;
        std::vector<std::string> keys;

        // Generate keys upfront
        keys.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            keys.push_back(std::format("rapid_key_{}", i));
        }

        std::uniform_int_distribution<size_t> key_dist(0, keys.size() - 1);

        // Rapid-fire mixed operations
        for (int op = 0; op < num_rapid_ops; ++op)
        {
            const std::string& key = keys[key_dist(rng_)];

            // Alternate between put and get
            if (op % 2 == 0)
            {
                std::vector<char> value = random_value(50, 200);
                auto result = co_await partition->put(std::string(key), std::move(value));
                EXPECT_TRUE(result.has_value());
            }
            else
            {
                auto result = co_await partition->get(key);
                EXPECT_TRUE(result.has_value());
            }
        }

        auto stats = partition->get_stats();

        // Verify high throughput was achieved
        EXPECT_EQ(stats.puts_total + stats.gets_total, num_rapid_ops);

        // All keys should be accessible
        for (const auto& key: keys)
        {
            auto get_result = co_await partition->get(key);
            // Key might not exist if never written, but operation should succeed
            EXPECT_TRUE(get_result.has_value());
        }
    };

    SyncWait(test_coro());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
