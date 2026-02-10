#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "bitcask/entry.hpp"
#include "bitcask/partition.hpp"
#include "kio/kio.hpp"

namespace fs = std::filesystem;

namespace
{
    std::vector<std::byte> BytesFromString(std::string_view value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(value.data());
        return std::vector(begin, begin + value.size());
    }

    std::string BytesToString(const std::vector<std::byte>& bytes)
    {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    fs::path MakeTempDir(std::string_view prefix)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path dir = fs::temp_directory_path() / std::format("{}_{}", prefix, now);
        fs::create_directories(dir);
        return dir;
    }
} // namespace

class PartitionTest : public ::testing::Test
{
protected:
    fs::path test_dir_;
    kio::IoContext ctx_;
    bitcask::BitcaskConfig config_;

    void SetUp() override
    {
        test_dir_ = MakeTempDir("bitcask_partition_test");
        fs::create_directories(test_dir_ / "partition_0");

        config_.directory = test_dir_;
        config_.max_file_size = 4 * 1024;
        config_.sync_on_write = true;
        config_.auto_compact = false;
        config_.fragmentation_threshold = 0.3;
    }

    void TearDown() override
    {
        fs::remove_all(test_dir_);
    }
};

TEST_F(PartitionTest, PutGetDel)
{
    auto task = [&]() -> kio::Task<>
    {
        auto open_res = co_await bitcask::Partition::AsyncOpen(ctx_, config_, 0);
        EXPECT_TRUE(open_res.has_value());
        if (!open_res)
        {
            co_return;
        }

        auto partition = std::move(open_res.value());

        const std::string value = "value";
        auto put_res = co_await partition->Put(ctx_, "key", std::as_bytes(std::span(value)));
        EXPECT_TRUE(put_res.has_value());

        auto get_res = co_await partition->Get(ctx_, "key");
        EXPECT_TRUE(get_res.has_value());
        if (get_res && get_res.value())
        {
            EXPECT_EQ(BytesToString(get_res.value().value()), value);
        }

        auto del_res = co_await partition->Del(ctx_, "key");
        EXPECT_TRUE(del_res.has_value());

        auto get_missing = co_await partition->Get(ctx_, "key");
        EXPECT_TRUE(get_missing.has_value());
        EXPECT_FALSE(get_missing.value().has_value());

        auto close_res = co_await partition->AsyncClose(ctx_);
        EXPECT_TRUE(close_res.has_value());
    };

    ctx_.RunUntilDone(task());
}

TEST_F(PartitionTest, RecoveryRespectsUpdatesAndDeletions)
{
    auto task = [&]() -> kio::Task<>
    {
        {
            auto open_res = co_await bitcask::Partition::AsyncOpen(ctx_, config_, 0);
            EXPECT_TRUE(open_res.has_value());
            if (!open_res)
            {
                co_return;
            }

            auto partition = std::move(open_res.value());

            auto old_value = BytesFromString("old");
            auto new_value = BytesFromString("new");
            auto keep_value = BytesFromString("value");
            auto tombstone_value = BytesFromString("tombstone");

            co_await partition->Put(ctx_, "key", std::span<const std::byte>(old_value));
            co_await partition->Put(ctx_, "key", std::span<const std::byte>(new_value));
            co_await partition->Put(ctx_, "keep", std::span<const std::byte>(keep_value));
            co_await partition->Del(ctx_, "deleted");
            co_await partition->Del(ctx_, "deleted");
            co_await partition->Put(ctx_, "deleted", std::span<const std::byte>(tombstone_value));
            co_await partition->Del(ctx_, "deleted");

            auto close_res = co_await partition->AsyncClose(ctx_);
            EXPECT_TRUE(close_res.has_value());
        }

        {
            auto open_res = co_await bitcask::Partition::AsyncOpen(ctx_, config_, 0);
            EXPECT_TRUE(open_res.has_value());
            if (!open_res)
            {
                co_return;
            }

            auto partition = std::move(open_res.value());

            auto get_key = co_await partition->Get(ctx_, "key");
            EXPECT_TRUE(get_key.has_value());
            if (get_key && get_key.value())
            {
                EXPECT_EQ(BytesToString(get_key.value().value()), "new");
            }

            auto get_keep = co_await partition->Get(ctx_, "keep");
            EXPECT_TRUE(get_keep.has_value());
            if (get_keep && get_keep.value())
            {
                EXPECT_EQ(BytesToString(get_keep.value().value()), "value");
            }

            auto get_deleted = co_await partition->Get(ctx_, "deleted");
            EXPECT_TRUE(get_deleted.has_value());
            EXPECT_FALSE(get_deleted.value().has_value());

            auto close_res = co_await partition->AsyncClose(ctx_);
            EXPECT_TRUE(close_res.has_value());
        }
    };

    ctx_.RunUntilDone(task());
}

TEST_F(PartitionTest, FileRotationCreatesMultipleFiles)
{
    auto task = [&]() -> kio::Task<>
    {
        bitcask::BitcaskConfig cfg = config_;
        cfg.max_file_size = 1024;

        auto open_res = co_await bitcask::Partition::AsyncOpen(ctx_, cfg, 0);
        EXPECT_TRUE(open_res.has_value());
        if (!open_res)
        {
            co_return;
        }

        auto partition = std::move(open_res.value());

        std::string payload(600, 'X');
        for (int i = 0; i < 10; ++i)
        {
            auto put_res = co_await partition->Put(ctx_, std::format("key_{}", i),
                                                   std::as_bytes(std::span(payload)));
            EXPECT_TRUE(put_res.has_value());
        }

        auto close_res = co_await partition->AsyncClose(ctx_);
        EXPECT_TRUE(close_res.has_value());

        size_t data_files = 0;
        for (const auto& entry : fs::directory_iterator(test_dir_ / "partition_0"))
        {
            if (entry.path().extension() == ".db" && entry.path().stem().string().starts_with("data_"))
            {
                ++data_files;
            }
        }
        EXPECT_GT(data_files, 1u);
    };

    ctx_.RunUntilDone(task());
}

TEST_F(PartitionTest, RecoveryIgnoresTruncatedTail)
{
    auto task = [&]() -> kio::Task<>
    {
        const std::string v1 = "value_1";
        const std::string v2 = "value_2";
        const std::string v3 = "value_3";

        // Phase 1: write a few entries and close cleanly
        {
            auto open_res = co_await bitcask::Partition::AsyncOpen(ctx_, config_, 0);
            EXPECT_TRUE(open_res.has_value());
            if (!open_res)
            {
                co_return;
            }

            auto partition = std::move(open_res.value());

            EXPECT_TRUE((co_await partition->Put(ctx_, "k1", std::as_bytes(std::span(v1)))).has_value());
            EXPECT_TRUE((co_await partition->Put(ctx_, "k2", std::as_bytes(std::span(v2)))).has_value());
            EXPECT_TRUE((co_await partition->Put(ctx_, "k3", std::as_bytes(std::span(v3)))).has_value());

            auto close_res = co_await partition->AsyncClose(ctx_);
            EXPECT_TRUE(close_res.has_value());
        }

        // Find the single data file
        fs::path data_path;
        for (const auto& entry : fs::directory_iterator(test_dir_ / "partition_0"))
        {
            if (entry.path().extension() == ".db" && entry.path().stem().string().starts_with("data_"))
            {
                data_path = entry.path();
                break;
            }
        }

        EXPECT_FALSE(data_path.empty()) << "Expected a data file to exist";
        if (data_path.empty())
        {
            co_return;
        }

        // Remove hint file so recovery must scan the data file
        const auto stem = data_path.stem().string(); // data_{id}
        const auto file_id_str = stem.substr(std::string("data_").size());
        const auto hint_path = test_dir_ / "partition_0" / std::format("hint_{}.ht", file_id_str);
        if (fs::exists(hint_path))
        {
            fs::remove(hint_path);
        }

        // Truncate into the last entry to simulate a crash
        bitcask::DataEntry e1("k1", std::as_bytes(std::span(v1)));
        bitcask::DataEntry e2("k2", std::as_bytes(std::span(v2)));
        bitcask::DataEntry e3("k3", std::as_bytes(std::span(v3)));

        const uint64_t valid_size = e1.Size() + e2.Size();
        const uint64_t partial_size = valid_size + 10; // partial header/body of entry 3
        fs::resize_file(data_path, partial_size);

        // Phase 2: reopen and verify only the first two entries are recovered
        {
            auto open_res = co_await bitcask::Partition::AsyncOpen(ctx_, config_, 0);
            EXPECT_TRUE(open_res.has_value());
            if (!open_res)
            {
                co_return;
            }

            auto partition = std::move(open_res.value());

            auto get1 = co_await partition->Get(ctx_, "k1");
            EXPECT_TRUE(get1.has_value());
            EXPECT_TRUE(get1.value().has_value());
            if (get1.value())
            {
                EXPECT_EQ(BytesToString(get1.value().value()), v1);
            }

            auto get2 = co_await partition->Get(ctx_, "k2");
            EXPECT_TRUE(get2.has_value());
            EXPECT_TRUE(get2.value().has_value());
            if (get2.value())
            {
                EXPECT_EQ(BytesToString(get2.value().value()), v2);
            }

            auto get3 = co_await partition->Get(ctx_, "k3");
            EXPECT_TRUE(get3.has_value());
            EXPECT_FALSE(get3.value().has_value());

            auto close_res = co_await partition->AsyncClose(ctx_);
            EXPECT_TRUE(close_res.has_value());
        }
    };

    ctx_.RunUntilDone(task());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
