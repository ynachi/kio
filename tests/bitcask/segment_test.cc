#include "bitcask/segment.hpp"

#include "uring/core/io.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

class SegmentManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto io = URing::IO(0, nullptr,
                            {
        },
                            {
                                {.size = 4096, .count = 256},
                                {.size = 8192, .count = 16},
                                {.size = 1024 * 1024 + 4096, .count = 2},
                            });
        io_ = std::make_unique<URing::IO>(std::move(io));

        fs::path temp_base = fs::temp_directory_path();
        test_dir_ = temp_base / "bitcask_test_dir_12345";
        fs::remove_all(test_dir_);
        fs::create_directories(test_dir_ / "partition_0");

        bitcask::BitcaskConfig cfg;
        cfg.directory = test_dir_;
        cfg.max_segment_size = 1024 * 1024;  // 1MB

        manager_ = std::make_unique<bitcask::SegmentManager>(0, cfg, 0, 0);
    }

    void TearDown() override { std::filesystem::remove_all(test_dir_); }

    std::unique_ptr<URing::IO> io_;
    std::filesystem::path test_dir_;
    std::unique_ptr<bitcask::SegmentManager> manager_;
};

TEST_F(SegmentManagerTest, SuccessfulAppend)
{
    auto test_coro = [&]() -> URing::Task<void>
    {
        const std::string key{"key"};
        std::string value{"value"};

        auto res = co_await manager_->append(*io_, key, std::as_bytes(std::span(value)));
        EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
        if (!res)
            co_return {};

        EXPECT_EQ(res.value(), 0);
        auto close_res = co_await manager_->close(*io_);
        EXPECT_TRUE(close_res.has_value());

        co_return {};
    };

    URing::sync_wait(*io_, test_coro());
}

TEST_F(SegmentManagerTest, RotationAndRetrieval)
{
    auto test_coro = [&]() -> URing::Task<void>
    {
        // 1. Write first entry to Segment 1
        std::string val1 = "value_in_segment_1";
        auto res1 = co_await manager_->append(*io_, "key1", val1);
        EXPECT_TRUE(res1.has_value());
        if (!res1)
            co_return {};
        uint64_t offset1 = *res1;

        // --- FS CHECK: Segment 1 should be pre-allocated to 1MB right now ---
        bitcask::BitcaskConfig cfg;
        cfg.directory = test_dir_;
        auto path1 = cfg.get_data_file_path(1, 0);
        EXPECT_EQ(fs::file_size(path1), 1024 * 1024);

        // 2. MANUALLY trigger rotation
        // This will call seal_active() which should truncate path1
        auto rot_res = co_await manager_->rotate(*io_);
        EXPECT_TRUE(rot_res.has_value());

        // --- FS CHECK: Segment 1 should now be SMALL (truncated) ---
        EXPECT_LT(fs::file_size(path1), 1024 * 1024);
        EXPECT_GT(fs::file_size(path1), 0);

        // 3. Write second entry (now in Segment 2)
        std::string val2 = "value_in_segment_2";
        auto res3 = co_await manager_->append(*io_, "key2", val2);
        EXPECT_TRUE(res3.has_value());
        if (!res3)
            co_return {};
        uint64_t offset3 = *res3;

        // Verify Segment 2 is ACTIVE (pre-allocated)
        auto path2 = cfg.get_data_file_path(2, 0);
        EXPECT_TRUE(fs::exists(path2));
        EXPECT_EQ(fs::file_size(path2), 1024 * 1024);

        // 4. Verify retrieval from Segment 1 (sealed)
        auto buf1 = io_->take_fixed_buffer(val1.size());
        EXPECT_TRUE(buf1.has_value());
        if (!buf1)
            co_return {};

        bitcask::ValueLocation loc1{
            .segment_id = 1, .value_len = static_cast<uint32_t>(val1.size()), .value_offset = offset1 + 32 + 4};

        auto read_res1 = co_await manager_->value_into(*io_, *buf1, loc1);
        EXPECT_TRUE(read_res1.has_value());
        if (read_res1)
        {
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf1->ptr()), val1.size()), val1);
        }

        // 5. Verify retrieval from Segment 2 (active)
        auto buf2 = io_->take_fixed_buffer(val2.size());
        EXPECT_TRUE(buf2.has_value());
        if (!buf2)
            co_return {};

        bitcask::ValueLocation loc2{
            .segment_id = 2, .value_len = static_cast<uint32_t>(val2.size()), .value_offset = offset3 + 32 + 4};

        auto read_res2 = co_await manager_->value_into(*io_, *buf2, loc2);
        EXPECT_TRUE(read_res2.has_value());
        if (read_res2)
        {
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf2->ptr()), val2.size()), val2);
        }

        auto close_res = co_await manager_->close(*io_);
        EXPECT_TRUE(close_res.has_value());

        co_return {};
    };

    URing::sync_wait(*io_, test_coro());
}

TEST_F(SegmentManagerTest, CacheEviction)
{
    bitcask::BitcaskConfig tiny_cfg;
    tiny_cfg.directory = test_dir_;
    tiny_cfg.max_segment_size = 1024;
    tiny_cfg.max_open_sealed_files = 2;

    auto tiny_manager = std::make_unique<bitcask::SegmentManager>(0, tiny_cfg, 0, 0);

    auto test_coro = [&]() -> URing::Task<void>
    {
        auto res1 = co_await tiny_manager->append(*io_, "k1", "val1");
        co_await tiny_manager->rotate(*io_);

        auto res2 = co_await tiny_manager->append(*io_, "k2", "val2");
        co_await tiny_manager->rotate(*io_);

        auto res3 = co_await tiny_manager->append(*io_, "k3", "val3");
        co_await tiny_manager->rotate(*io_);

        auto buf = io_->take_fixed_buffer(10);
        EXPECT_TRUE(buf.has_value());
        if (!buf)
        {
            co_return {};
        }

        // 3. Access Segment 1 -> Added to cache
        bitcask::ValueLocation loc1{.segment_id = 1, .value_len = 4, .value_offset = *res1 + 32 + 2};
        EXPECT_TRUE((co_await tiny_manager->value_into(*io_, *buf, loc1)).has_value());

        // 4. Access Segment 2 -> Added to cache
        bitcask::ValueLocation loc2{.segment_id = 2, .value_len = 4, .value_offset = *res2 + 32 + 2};
        EXPECT_TRUE((co_await tiny_manager->value_into(*io_, *buf, loc2)).has_value());

        // 5. Access Segment 3 -> Cache is full (limit 2), evicts Segment 1
        bitcask::ValueLocation loc3{.segment_id = 3, .value_len = 4, .value_offset = *res3 + 32 + 2};
        EXPECT_TRUE((co_await tiny_manager->value_into(*io_, *buf, loc3)).has_value());

        auto final_res = co_await tiny_manager->value_into(*io_, *buf, loc1);
        EXPECT_TRUE(final_res.has_value());
        if (final_res)
        {
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf->ptr()), 4), "val1");
        }

        co_await tiny_manager->close(*io_);
        co_return {};
    };

    URing::sync_wait(*io_, test_coro());
}

TEST_F(SegmentManagerTest, CacheConsistency)
{
    auto test_coro = [&]() -> URing::Task<void>
    {
        // 1. Write and rotate to Segment 1
        std::string val = "consistency_test";
        auto res = co_await manager_->append(*io_, "key", val);
        EXPECT_TRUE(res.has_value());
        if (!res)
            co_return {};

        uint64_t offset = *res;
        co_await manager_->rotate(*io_);

        // 2. Read once to populate cache
        auto buf = io_->take_fixed_buffer(val.size());
        EXPECT_TRUE(buf.has_value());
        if (!buf)
            co_return {};

        bitcask::ValueLocation loc{
            .segment_id = 1, .value_len = static_cast<uint32_t>(val.size()), .value_offset = offset + 32 + 3};

        auto read_res1 = co_await manager_->value_into(*io_, *buf, loc);
        EXPECT_TRUE(read_res1.has_value());

        // 3. DELETE the file from disk!
        bitcask::BitcaskConfig cfg;
        cfg.directory = test_dir_;
        fs::remove(cfg.get_data_file_path(1, 0));

        // 4. Read again
        // If the cache is working, it should still have the FD open and succeed.
        // If it's not caching, it will try to open() the deleted file and fail.
        // Note: It works because linux do not completely remove the file on unlink unless the ref count is 0.
        auto read_res2 = co_await manager_->value_into(*io_, *buf, loc);
        EXPECT_TRUE(read_res2.has_value()) << "Should have used cached FD even if file is deleted";
        if (read_res2)
        {
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf->ptr()), val.size()), val);
        }

        co_await manager_->close(*io_);
        co_return {};
    };

    URing::sync_wait(*io_, test_coro());
}

TEST_F(SegmentManagerTest, CorruptionDetection)
{
    auto test_coro = [&]() -> URing::Task<void>
    {
        std::string val = "integrity_test";
        auto res = co_await manager_->append(*io_, "key", val);
        EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
        if (!res)
        {
            co_return {};
        }
        const uint64_t record_offset = *res;

        auto close_res = co_await manager_->close(*io_);
        EXPECT_TRUE(close_res.has_value()) << (close_res ? "" : close_res.error().message());
        if (!close_res)
        {
            co_return {};
        }

        auto verify_res1 = co_await manager_->verify_entry(*io_, 1, record_offset);
        EXPECT_TRUE(verify_res1.has_value()) << (verify_res1 ? "" : verify_res1.error().message());
        if (!verify_res1)
        {
            co_return {};
        }

        bitcask::BitcaskConfig cfg;
        cfg.directory = test_dir_;
        auto path = cfg.get_data_file_path(1, 0);

        {
            std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
            EXPECT_TRUE(file.is_open());
            if (!file.is_open())
            {
                co_return {};
            }

            char byte = 0;
            file.seekg(static_cast<std::streamoff>(record_offset));
            file.read(&byte, 1);
            EXPECT_TRUE(file.good());
            if (!file.good())
            {
                co_return {};
            }

            byte ^= static_cast<char>(0xFF);
            file.seekp(static_cast<std::streamoff>(record_offset));
            file.write(&byte, 1);
            EXPECT_TRUE(file.good());
            if (!file.good())
            {
                co_return {};
            }
        }

        auto verify_res2 = co_await manager_->verify_entry(*io_, 1, record_offset);
        EXPECT_FALSE(verify_res2.has_value()) << "Corruption was not detected";
        if (!verify_res2)
        {
            EXPECT_EQ(verify_res2.error(), std::make_error_code(std::errc::bad_message));
        }

        co_return {};
    };

    URing::sync_wait(*io_, test_coro());
}

TEST_F(SegmentManagerTest, StressAppendRotateAndReadBack)
{
    auto test_coro = [&]() -> URing::Task<void>
    {
        struct Entry
        {
            bitcask::SegmentId segment_id;
            uint64_t offset;
            std::string key;
            std::string value;
        };

        constexpr int kSegments = 4;
        constexpr int kEntriesPerSegment = 32;
        std::vector<Entry> entries;
        entries.reserve(kSegments * kEntriesPerSegment);

        for (int segment = 1; segment <= kSegments; ++segment)
        {
            for (int i = 0; i < kEntriesPerSegment; ++i)
            {
                const int ordinal = (segment - 1) * kEntriesPerSegment + i;
                std::string key = "key_" + std::to_string(ordinal);
                std::string value = "value_" + std::to_string(ordinal) + std::string(static_cast<size_t>(ordinal % 17), 'x');

                auto append_res = co_await manager_->append(*io_, key, value);
                EXPECT_TRUE(append_res.has_value()) << (append_res ? "" : append_res.error().message());
                if (!append_res)
                {
                    co_return {};
                }

                entries.push_back(Entry{
                    .segment_id = static_cast<bitcask::SegmentId>(segment),
                    .offset = *append_res,
                    .key = std::move(key),
                    .value = std::move(value),
                });
            }

            if (segment != kSegments)
            {
                auto rotate_res = co_await manager_->rotate(*io_);
                EXPECT_TRUE(rotate_res.has_value()) << (rotate_res ? "" : rotate_res.error().message());
                if (!rotate_res)
                {
                    co_return {};
                }
            }
        }

        const std::array<size_t, 8> samples{
            0,
            7,
            31,
            32,
            63,
            64,
            entries.size() - 2,
            entries.size() - 1,
        };

        for (const size_t sample : samples)
        {
            const auto& entry = entries[sample];
            auto buf = io_->take_fixed_buffer(entry.value.size());
            EXPECT_TRUE(buf.has_value()) << (buf ? "" : buf.error().message());
            if (!buf)
            {
                co_return {};
            }

            bitcask::ValueLocation loc{
                .segment_id = entry.segment_id,
                .value_len = static_cast<uint32_t>(entry.value.size()),
                .value_offset = entry.offset + sizeof(bitcask::LogEntryHeader) + entry.key.size(),
            };

            auto read_res = co_await manager_->value_into(*io_, *buf, loc);
            EXPECT_TRUE(read_res.has_value()) << (read_res ? "" : read_res.error().message());
            if (!read_res)
            {
                co_return {};
            }

            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf->ptr()), entry.value.size()), entry.value);
        }

        auto close_res = co_await manager_->close(*io_);
        EXPECT_TRUE(close_res.has_value()) << (close_res ? "" : close_res.error().message());

        co_return {};
    };

    URing::sync_wait(*io_, test_coro());
}
