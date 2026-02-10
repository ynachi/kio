#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
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

    std::vector<std::string> ListDataFiles(const fs::path& partition_dir)
    {
        std::vector<std::string> files;
        if (!fs::exists(partition_dir))
        {
            return files;
        }

        for (const auto& entry : fs::directory_iterator(partition_dir))
        {
            if (entry.path().extension() == ".db" && entry.path().stem().string().starts_with("data_"))
            {
                files.push_back(entry.path().filename().string());
            }
        }

        std::ranges::sort(files);
        return files;
    }
} // namespace

class CompactionTest : public ::testing::Test
{
protected:
    fs::path test_dir_;
    kio::IoContext ctx_;
    bitcask::BitcaskConfig config_;

    void SetUp() override
    {
        kio::alog::g_level = kio::alog::Level::Debug;
        test_dir_ = MakeTempDir("bitcask_compaction_test");
        fs::create_directories(test_dir_ / "partition_0");

        config_.directory = test_dir_;
        config_.max_file_size = 1024;
        config_.sync_on_write = true;
        config_.auto_compact = false;
        config_.fragmentation_threshold = 0.2;
    }

    void TearDown() override
    {
        fs::remove_all(test_dir_);
    }
};

TEST_F(CompactionTest, CompactionChangesDataFilesWhenFragmented)
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

        std::string payload(600, 'X');
        for (int i = 0; i < 20; ++i)
        {
            auto put_res = co_await partition->Put(ctx_, std::format("key_{}", i),
                                                   std::as_bytes(std::span(payload)));
            if (!put_res.has_value())
            {
                ALOG_ERROR("{}", put_res.error().message());
            }
            EXPECT_TRUE(put_res.has_value());
        }

        for (int i = 0; i < 10; ++i)
        {
            auto put_res = co_await partition->Put(ctx_, std::format("key_{}", i),
                                                   std::as_bytes(std::span(payload)));
            if (!put_res.has_value())
            {
                ALOG_ERROR("{}", put_res.error().message());
            }
            EXPECT_TRUE(put_res.has_value());
        }

        const auto before = ListDataFiles(test_dir_ / "partition_0");
        EXPECT_GT(before.size(), 1u);

        auto compact_res = co_await partition->Compact(ctx_);
        if (!compact_res.has_value())
        {
            ALOG_ERROR("{}", compact_res.error().message());
        }
        EXPECT_TRUE(compact_res.has_value());

        const auto after = ListDataFiles(test_dir_ / "partition_0");
        EXPECT_NE(before, after);

        auto close_res = co_await partition->AsyncClose(ctx_);
        if (!close_res.has_value())
        {
            ALOG_ERROR("{}", close_res.error().message());
        }
        EXPECT_TRUE(close_res.has_value());
    };

    ctx_.RunUntilDone(task());
}

TEST_F(CompactionTest, CompactionDoesNotLoseLiveData)
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

        std::string payload(600, 'X');
        for (int i = 0; i < 15; ++i)
        {
            auto put_res = co_await partition->Put(ctx_, std::format("key_{}", i),
                                                   std::as_bytes(std::span(payload)));
            EXPECT_TRUE(put_res.has_value());
        }

        for (int i = 0; i < 5; ++i)
        {
            auto put_res = co_await partition->Put(ctx_, std::format("key_{}", i),
                                                   std::as_bytes(std::span(payload)));
            EXPECT_TRUE(put_res.has_value());
        }

        auto compact_res = co_await partition->Compact(ctx_);
        EXPECT_TRUE(compact_res.has_value());

        for (int i = 0; i < 15; ++i)
        {
            auto get_res = co_await partition->Get(ctx_, std::format("key_{}", i));
            EXPECT_TRUE(get_res.has_value());
            if (get_res.has_value())
            {
                EXPECT_TRUE(get_res.value().has_value());
            }
        }

        auto close_res = co_await partition->AsyncClose(ctx_);
        EXPECT_TRUE(close_res.has_value());
    };

    ctx_.RunUntilDone(task());
}
