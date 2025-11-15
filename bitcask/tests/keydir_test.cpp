//
// Created by Yao ACHI on 08/11/2025.
//
#include "bitcask/include/keydir.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace bitcask;

// Helper to create a dummy ValueLocation
ValueLocation make_loc(const uint32_t file_id, const uint64_t timestamp) { return ValueLocation{file_id, 100, 50, timestamp}; }

class KeyDirTest : public ::testing::Test
{
protected:
    KeyDir keydir{4};
};

TEST_F(KeyDirTest, BasicPutGet)
{
    const auto loc1 = make_loc(1, 100);
    keydir.put("key1", loc1);

    const auto res = keydir.get("key1");
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->file_id, loc1.file_id);
    EXPECT_EQ(res->timestamp, loc1.timestamp);

    const auto res_missing = keydir.get("missing_key");
    EXPECT_FALSE(res_missing.has_value());
}

TEST_F(KeyDirTest, PutOverwritesExisting)
{
    const auto loc1 = make_loc(1, 100);
    const auto loc2 = make_loc(2, 200);  // Newer timestamp

    keydir.put("key1", loc1);
    const auto res1 = keydir.get("key1");
    EXPECT_EQ(res1->timestamp, 100);

    // Overwrite with newer
    keydir.put("key1", loc2);
    const auto res2 = keydir.get("key1");
    EXPECT_EQ(res2->timestamp, 200);

    // Overwrite with OLDER (should still overwrite for a normal put!)
    const auto loc3 = make_loc(1, 50);
    keydir.put("key1", loc3);
    const auto res3 = keydir.get("key1");
    EXPECT_EQ(res3->timestamp, 50);
}

TEST_F(KeyDirTest, PutIfNewerLogic)
{
    const auto loc_old = make_loc(1, 100);
    const auto loc_new = make_loc(2, 200);

    // Case A: New key
    keydir.put_if_newer("keyA", loc_old);
    EXPECT_EQ(keydir.get("keyA")->timestamp, 100);

    // Case B: Newer timestamp replaces it older
    keydir.put_if_newer("keyA", loc_new);
    EXPECT_EQ(keydir.get("keyA")->timestamp, 200);

    // Case C: Older timestamp DOES NOT replace it newer
    keydir.put_if_newer("keyA", loc_old);
    EXPECT_EQ(keydir.get("keyA")->timestamp, 200);
}

TEST_F(KeyDirTest, KeysGoToDifferentShards)
{
    // We can't easily inspect shards directly since they are private.
    // But we can verify that many keys can be stored and retrieved.
    for (int i = 0; i < 1000; ++i)
    {
        std::string key = std::format("key_{}", i);
        keydir.put(std::move(key), make_loc(1, i));
    }

    for (int i = 0; i < 1000; ++i)
    {
        std::string key = std::format("key_{}", i);
        auto res = keydir.get(key);
        ASSERT_TRUE(res.has_value()) << "Missing key: " << key;
        EXPECT_EQ(res->timestamp, static_cast<uint64_t>(i));
    }
}

TEST_F(KeyDirTest, SnapshotProvidesConsistentCopy)
{
    keydir.put("a", make_loc(1, 10));
    keydir.put("b", make_loc(1, 20));
    keydir.put("c", make_loc(1, 30));

    const auto snap = keydir.snapshot();
    EXPECT_EQ(snap.size(), 3);
    EXPECT_EQ(snap.at("a").timestamp, 10);
    EXPECT_EQ(snap.at("b").timestamp, 20);
    EXPECT_EQ(snap.at("c").timestamp, 30);

    // Modifying KeyDir after a snapshot doesn't affect the snapshot
    keydir.put("d", make_loc(1, 40));
    EXPECT_EQ(snap.size(), 3);
    EXPECT_FALSE(snap.contains("d"));
}

TEST_F(KeyDirTest, ConcurrentAccessCorrectness)
{
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 1000;

    std::vector<std::jthread> threads;
    threads.reserve(num_threads);

    // Phase 1: Concurrent writes to different keys
    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
                [this, t]()
                {
                    for (int i = 0; i < ops_per_thread; ++i)
                    {
                        std::string key = std::format("thread_{}_key_{}", t, i);
                        keydir.put(std::move(key), make_loc(t, i));
                    }
                });
    }

    for (auto& th: threads)
    {
        th.join();
    }
    threads.clear();

    // Verify all keys were written correctly
    for (int t = 0; t < num_threads; ++t)
    {
        for (int i = 0; i < ops_per_thread; ++i)
        {
            std::string key = std::format("thread_{}_key_{}", t, i);
            auto res = keydir.get(key);
            ASSERT_TRUE(res.has_value()) << "Missing key: " << key;
            EXPECT_EQ(res->file_id, static_cast<uint32_t>(t));
            EXPECT_EQ(res->timestamp, static_cast<uint64_t>(i));
        }
    }

    // Phase 2: Concurrent updates to same keys
    constexpr int shared_keys = 100;
    for (int i = 0; i < shared_keys; ++i)
    {
        std::string key = std::format("shared_{}", i);
        keydir.put(std::move(key), make_loc(0, 0));
    }

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
                [this, t]()
                {
                    for (int i = 0; i < shared_keys; ++i)
                    {
                        std::string key = std::format("shared_{}", i);
                        keydir.put(std::move(key), make_loc(t, t * 1000 + i));
                    }
                });
    }

    for (auto& th: threads)
    {
        th.join();
    }
    threads.clear();

    // Verify all shared keys have valid values (some thread's write succeeded)
    for (int i = 0; i < shared_keys; ++i)
    {
        std::string key = std::format("shared_{}", i);
        auto res = keydir.get(key);
        ASSERT_TRUE(res.has_value()) << "Missing shared key: " << key;
        EXPECT_LT(res->file_id, static_cast<uint32_t>(num_threads));
    }

    // Phase 3: Concurrent put_if_newer with ordering guarantees
    constexpr int timestamp_test_keys = 50;

    // Initialize with timestamp 5000
    for (int i = 0; i < timestamp_test_keys; ++i)
    {
        std::string key = std::format("ts_{}", i);
        keydir.put(std::move(key), make_loc(99, 5000));
    }

    // Spawn threads trying to update with various timestamps
    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
                [this, t]()
                {
                    std::mt19937 rng(t);
                    std::uniform_int_distribution<uint64_t> dist(0, 10000);

                    for (int i = 0; i < timestamp_test_keys; ++i)
                    {
                        std::string key = std::format("ts_{}", i);
                        uint64_t timestamp = dist(rng);
                        keydir.put_if_newer(std::move(key), make_loc(t, timestamp));
                    }
                });
    }

    for (auto& th: threads)
    {
        th.join();
    }
    threads.clear();

    // Verify final timestamps are >= 5000 (monotonically increasing via put_if_newer)
    for (int i = 0; i < timestamp_test_keys; ++i)
    {
        std::string key = std::format("ts_{}", i);
        auto res = keydir.get(key);
        ASSERT_TRUE(res.has_value()) << "Missing timestamp key: " << key;
        EXPECT_GE(res->timestamp, 5000ULL) << "Timestamp went backwards for key: " << key;
    }

    // Phase 4: Concurrent reads and writes
    std::atomic<bool> keep_running{true};
    std::atomic<int> read_errors{0};

    // Writer threads
    for (int t = 0; t < num_threads / 2; ++t)
    {
        threads.emplace_back(
                [this, &keep_running, t]()
                {
                    int counter = 0;
                    while (keep_running)
                    {
                        std::string key = std::format("rw_{}", t % 10);
                        keydir.put(std::move(key), make_loc(t, counter++));
                    }
                });
    }

    // Reader threads
    for (int t = num_threads / 2; t < num_threads; ++t)
    {
        threads.emplace_back(
                [this, &keep_running, &read_errors]()
                {
                    while (keep_running)
                    {
                        for (int i = 0; i < 10; ++i)
                        {
                            std::string key = std::format("rw_{}", i);
                            // Key might not exist yet, but if it does, it should be valid
                            if (auto res = keydir.get(key); res.has_value() && res->file_id >= num_threads / 2)
                            {
                                read_errors.fetch_add(1);
                            }
                        }
                    }
                });
    }

    // Let them run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    keep_running = false;

    for (auto& th: threads)
    {
        th.join();
    }

    EXPECT_EQ(read_errors.load(), 0) << "Concurrent reads observed invalid data";
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
