#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/partition.hpp"
#include "kio/kio.hpp"

namespace fs = std::filesystem;

namespace
{
    fs::path MakeTempDir(std::string_view prefix)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path dir = fs::temp_directory_path() / std::format("{}_{}", prefix, now);
        fs::create_directories(dir);
        return dir;
    }

    bool PerfEnabled()
    {
        const char* value = std::getenv("BITCASK_PERF");
        return value != nullptr && std::string_view(value) != "0";
    }

    uint64_t GetEnvU64(const char* name, uint64_t fallback)
    {
        const char* value = std::getenv(name);
        if (!value || *value == '\0')
        {
            return fallback;
        }

        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 10);
        if (!end || *end != '\0')
        {
            return fallback;
        }
        return parsed;
    }

} // namespace

class PerfStressTest : public ::testing::Test
{
protected:
    fs::path test_dir_;
    kio::IoContext ctx_;
    bitcask::BitcaskConfig config_;

    void SetUp() override
    {
        kio::alog::g_level = kio::alog::Level::Disabled;
        test_dir_ = MakeTempDir("bitcask_perf_test");
        fs::create_directories(test_dir_ / "partition_0");

        config_.directory = test_dir_;
        config_.sync_on_write = false;
        config_.auto_compact = false;
        config_.max_file_size = 64 * 1024 * 1024;
    }

    void TearDown() override
    {
        fs::remove_all(test_dir_);
    }
};

TEST_F(PerfStressTest, WriteThroughput)
{
    if (!PerfEnabled())
    {
        GTEST_SKIP() << "Set BITCASK_PERF=1 to run perf tests";
    }

    auto task = [&]() -> kio::Task<>
    {
        const uint64_t key_count = GetEnvU64("BITCASK_PERF_KEYS", 50000);
        const uint64_t value_size = GetEnvU64("BITCASK_PERF_VALUE_BYTES", 1024);

        auto open_res = co_await bitcask::Partition::Open(ctx_, config_, 0);
        EXPECT_TRUE(open_res.has_value());
        if (!open_res)
        {
            co_return;
        }
        auto partition = std::move(open_res.value());

        std::string payload(value_size, 'X');
        auto start = std::chrono::steady_clock::now();

        for (uint64_t i = 0; i < key_count; ++i)
        {
            auto put_res = co_await partition->Put(ctx_, std::format("key_{}", i),
                                                   std::as_bytes(std::span(payload)));
            EXPECT_TRUE(put_res.has_value());
        }

        auto end = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(end - start).count();
        const double ops_per_sec = seconds > 0.0 ? static_cast<double>(key_count) / seconds : 0.0;

        std::cout << "[perf] WriteThroughput: " << key_count << " ops in " << seconds
                  << "s (" << ops_per_sec << " ops/s)\n";

        auto close_res = co_await partition->AsyncClose(ctx_);
        EXPECT_TRUE(close_res.has_value());
    };

    ctx_.RunUntilDone(task());
}

TEST_F(PerfStressTest, RandomReadThroughput)
{
    if (!PerfEnabled())
    {
        GTEST_SKIP() << "Set BITCASK_PERF=1 to run perf tests";
    }

    auto task = [&]() -> kio::Task<>
    {
        const uint64_t key_count = GetEnvU64("BITCASK_PERF_KEYS", 50000);
        const uint64_t read_count = GetEnvU64("BITCASK_PERF_READS", 100000);
        const uint64_t value_size = GetEnvU64("BITCASK_PERF_VALUE_BYTES", 1024);

        auto open_res = co_await bitcask::Partition::Open(ctx_, config_, 0);
        EXPECT_TRUE(open_res.has_value());
        if (!open_res)
        {
            co_return;
        }
        auto partition = std::move(open_res.value());

        std::string payload(value_size, 'X');
        for (uint64_t i = 0; i < key_count; ++i)
        {
            auto put_res = co_await partition->Put(ctx_, std::format("key_{}", i),
                                                   std::as_bytes(std::span(payload)));
            EXPECT_TRUE(put_res.has_value());
        }

        std::mt19937_64 rng(42);
        std::uniform_int_distribution<uint64_t> dist(0, key_count - 1);

        auto start = std::chrono::steady_clock::now();
        uint64_t hits = 0;

        for (uint64_t i = 0; i < read_count; ++i)
        {
            const auto key = std::format("key_{}", dist(rng));
            auto get_res = co_await partition->Get(ctx_, key);
            EXPECT_TRUE(get_res.has_value());
            if (get_res.has_value() && get_res.value().has_value())
            {
                hits++;
            }
        }

        auto end = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(end - start).count();
        const double ops_per_sec = seconds > 0.0 ? static_cast<double>(read_count) / seconds : 0.0;

        std::cout << "[perf] RandomReadThroughput: " << read_count << " ops in " << seconds
                  << "s (" << ops_per_sec << " ops/s), hits=" << hits << "\n";

        EXPECT_EQ(hits, read_count);

        auto close_res = co_await partition->AsyncClose(ctx_);
        EXPECT_TRUE(close_res.has_value());
    };

    ctx_.RunUntilDone(task());
}

TEST_F(PerfStressTest, MixedReadWriteDeleteWorkload)
{
    if (!PerfEnabled())
    {
        GTEST_SKIP() << "Set BITCASK_PERF=1 to run perf tests";
    }

    auto task = [&]() -> kio::Task<>
    {
        const uint64_t initial_keys = GetEnvU64("BITCASK_PERF_KEYS", 20000);
        const uint64_t op_count = GetEnvU64("BITCASK_PERF_OPS", 100000);
        const uint64_t value_size = GetEnvU64("BITCASK_PERF_VALUE_BYTES", 512);

        auto open_res = co_await bitcask::Partition::Open(ctx_, config_, 0);
        EXPECT_TRUE(open_res.has_value());
        if (!open_res)
        {
            co_return;
        }
        auto partition = std::move(open_res.value());

        std::string payload(value_size, 'X');
        std::vector<std::string> live_keys;
        live_keys.reserve(initial_keys + op_count / 2);

        for (uint64_t i = 0; i < initial_keys; ++i)
        {
            live_keys.push_back(std::format("key_{}", i));
            auto put_res = co_await partition->Put(ctx_, live_keys.back(), std::as_bytes(std::span(payload)));
            EXPECT_TRUE(put_res.has_value());
        }

        std::mt19937_64 rng(123);
        std::uniform_int_distribution<int> op_dist(0, 99);
        uint64_t next_key_id = initial_keys;

        auto start = std::chrono::steady_clock::now();
        uint64_t reads = 0;
        uint64_t writes = 0;
        uint64_t deletes = 0;

        for (uint64_t i = 0; i < op_count; ++i)
        {
            const int op = op_dist(rng);
            if (op < 50 && !live_keys.empty())
            {
                const size_t idx = static_cast<size_t>(rng() % live_keys.size());
                auto get_res = co_await partition->Get(ctx_, live_keys[idx]);
                EXPECT_TRUE(get_res.has_value());
                reads++;
            }
            else if (op < 85)
            {
                const auto key = std::format("key_{}", next_key_id++);
                live_keys.push_back(key);
                auto put_res = co_await partition->Put(ctx_, key, std::as_bytes(std::span(payload)));
                EXPECT_TRUE(put_res.has_value());
                writes++;
            }
            else if (!live_keys.empty())
            {
                const size_t idx = static_cast<size_t>(rng() % live_keys.size());
                auto del_res = co_await partition->Del(ctx_, live_keys[idx]);
                EXPECT_TRUE(del_res.has_value());
                live_keys[idx] = live_keys.back();
                live_keys.pop_back();
                deletes++;
            }
        }

        auto end = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(end - start).count();
        const double ops_per_sec = seconds > 0.0 ? static_cast<double>(op_count) / seconds : 0.0;

        std::cout << "[perf] MixedWorkload: " << op_count << " ops in " << seconds
                  << "s (" << ops_per_sec << " ops/s)"
                  << " reads=" << reads << " writes=" << writes << " deletes=" << deletes << "\n";

        auto close_res = co_await partition->AsyncClose(ctx_);
        EXPECT_TRUE(close_res.has_value());
    };

    ctx_.RunUntilDone(task());
}
