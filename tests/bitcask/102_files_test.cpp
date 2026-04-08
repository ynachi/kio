#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include "bitcask/files.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

TEST(FileIDTest, EncodeDecode)
{
    FileID original{.partition = 1, .timestamp_sec = 1700000000, .sequence = 123};

    uint64_t encoded = original.Encode();
    FileID decoded = FileID::Decode(encoded);

    EXPECT_EQ(decoded.partition, original.partition);
    EXPECT_EQ(decoded.timestamp_sec, original.timestamp_sec);
    EXPECT_EQ(decoded.sequence, original.sequence);
}

TEST(FileIDTest, SortOrder)
{
    // ID A: Time 100, Seq 1
    FileID id_a{.partition = 1, .timestamp_sec = 100, .sequence = 1};
    // ID B: Time 100, Seq 2
    FileID id_b{.partition = 1, .timestamp_sec = 100, .sequence = 2};
    // ID C: Time 101, Seq 0
    FileID id_c{.partition = 1, .timestamp_sec = 101, .sequence = 0};

    EXPECT_TRUE(FileIdCompareByTime(id_a.Encode(), id_b.Encode()));
    EXPECT_TRUE(FileIdCompareByTime(id_b.Encode(), id_c.Encode()));
    EXPECT_FALSE(FileIdCompareByTime(id_b.Encode(), id_a.Encode()));
}

TEST(FileIDTest, Generator_Monotonicity)
{
    FileIdGenerator gen(1);

    uint64_t id1 = gen.Next();
    uint64_t id2 = gen.Next();

    FileID decoded1 = FileID::Decode(id1);
    FileID decoded2 = FileID::Decode(id2);

    // If generated in same second
    if (decoded1.timestamp_sec == decoded2.timestamp_sec)
    {
        EXPECT_EQ(decoded2.sequence, decoded1.sequence + 1);
    }
    else
    {
        EXPECT_GT(decoded2.timestamp_sec, decoded1.timestamp_sec);
        EXPECT_EQ(decoded2.sequence, 0);
    }
}

TEST(FileIDTest, Generator_StateRecovery)
{
    FileIdGenerator gen(1);

    // Simulate finding a file with high timestamp/sequence
    gen.UpdateState(2000000000, 50);

    // Next ID should respect the restored state
    // We can't easily force the system clock to be 2000000000,
    // but we can verify Next() doesn't produce an ID < restored state if clock is 'behind'
    // Note: The Generator implementation uses system_clock.
    // If system_clock is < 2000000000, it treats it as clock skew and uses last_timestamp_.

    uint64_t next_id = gen.Next();
    FileID decoded = FileID::Decode(next_id);

    EXPECT_EQ(decoded.timestamp_sec, 2000000000);
    EXPECT_EQ(decoded.sequence, 51);
}


// For this code to compile, I will assume a class 'TestRuntime' exists
class FileTest : public ::testing::Test
{
protected:
    fs::path temp_dir;
    kio::IoContext ctx;
    BitcaskConfig config;

    void SetUp() override
    {
        temp_dir = fs::temp_directory_path() / "bitcask_test_env";
        fs::create_directories(temp_dir);
    }

    void TearDown() override
    {
        fs::remove_all(temp_dir);
    }
};

// FDc cache tests
TEST_F(FileTest, FDCache_OpenAndEvict)
{
    // Create dummy files
    auto p1 = temp_dir / "1.db";
    auto p2 = temp_dir / "2.db";
    auto p3 = temp_dir / "3.db";
    {
        std::ofstream(p1) << "data";
        std::ofstream(p2) << "data";
        std::ofstream(p3) << "data";
    }

    auto task = [&](kio::IoContext& io) -> kio::Task<void>
    {
        // Cache size 2
        FDCache cache(2);

        // Open 1 (Miss -> Open)
        auto fd1 = co_await cache.GetOrOpen(io, 1, p1);
        EXPECT_TRUE(fd1.has_value());
        EXPECT_GT(fd1.value()->Get(), 0);
        EXPECT_EQ(cache.Size(), 1);

        // Open 2 (Miss -> Open)
        auto fd2 = co_await cache.GetOrOpen(io, 2, p2);
        EXPECT_TRUE(fd2.has_value());
        EXPECT_GT(fd2.value()->Get(), 0);
        EXPECT_EQ(cache.Size(), 2);

        // Open 3 (Miss -> Evict 1 -> Open 3)
        auto fd3 = co_await cache.GetOrOpen(io, 3, p3);
        EXPECT_TRUE(fd3.has_value());
        EXPECT_GT(fd3.value()->Get(), 0);
        EXPECT_EQ(cache.Size(), 2);

        // Open 1 again (Miss -> Evict 2 (LRU) -> Open 1)
        auto fd1_new = co_await cache.GetOrOpen(io, 1, p1);
        EXPECT_TRUE(fd1_new.has_value());
        EXPECT_GT(fd1_new.value()->Get(), 0);

        // If the cache works, fd1 and fd1_new might be different or same
        // depending on OS fd recycling, but the cache size must remain 2.
        EXPECT_EQ(cache.Size(), 2);

        co_return;
    }(ctx);

    ctx.RunUntilDone(std::move(task));
}


TEST_F(FileTest, BasicWrite)
{
    auto path = temp_dir / "d001.db";
    const int fd = open(path.c_str(), config.write_flags, config.file_mode);
    ASSERT_GE(fd, 0) << "Failed to create test file";

    auto shared_fd = std::make_shared<kio::FD>(fd);
    DataFile df = DataFile(shared_fd, 1, config);

    auto test = [&](kio::IoContext& io) -> kio::Task<>
    {
        std::string val = "long_value";
        const DataEntry entry("long_key", std::as_bytes(std::span(val)));
        const auto result = co_await df.AsyncWrite(io, entry);
        EXPECT_TRUE(result.has_value());

        // AsyncWrite returns the offset where entry was written
        const uint64_t offset = result.value();
        EXPECT_EQ(offset, 0) << "First entry should be at offset 0";
        EXPECT_EQ(df.Size(), entry.Size()) << "File size should match entry size";

        // check writes preserve offset
        auto expected_offset = entry.Size();
        const auto result2 = co_await df.AsyncWrite(io, entry);
        EXPECT_TRUE(result2.has_value());
        EXPECT_EQ(result2.value(), expected_offset);
        EXPECT_EQ(df.Size(), 2* entry.Size()) << "File size should match entry size X2";
    }(ctx);

    ctx.RunUntilDone(std::move(test));
}

TEST_F(FileTest, SequentialAsyncWrites)
{
    // Test that multiple coroutines writing to the SAME DataFile
    // don't overlap (space reservation prevents races)

    auto path = temp_dir / "concurrent.db";
    const int fd = open(path.c_str(), config.write_flags, config.file_mode);
    ASSERT_GE(fd, 0) << "Failed to create test file";

    auto shared_fd = std::make_shared<kio::FD>(fd);
    DataFile df = DataFile(shared_fd, 1, config);

    auto test = [&](kio::IoContext& io) -> kio::Task<>
    {
        constexpr int kNumWrites = 20;
        std::vector<DataEntry> entries;
        std::vector<size_t> sizes;

        // Prepare entries (keep them alive for the async operations)
        for (int i = 0; i < kNumWrites; ++i)
        {
            std::string val(50, 'X');
            entries.emplace_back(std::format("key_{}", i), std::as_bytes(std::span(val)));
            sizes.push_back(entries.back().Size());
        }

        // Launch concurrent writes
        std::vector<kio::Task<kio::Result<uint64_t>>> tasks;
        for (int i = 0; i < kNumWrites; ++i)
        {
            tasks.push_back(df.AsyncWrite(io, entries[i]));
        }

        // Collect offsets
        std::vector<uint64_t> offsets;
        for (auto& task : tasks)
        {
            auto result = co_await std::move(task);
            EXPECT_TRUE(result.has_value());
            offsets.push_back(result.value());
        }

        // Verify NO overlapping regions
        std::vector<std::pair<uint64_t, uint64_t>> ranges; // (start, end)
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            ranges.emplace_back(offsets[i], offsets[i] + sizes[i]);
        }

        std::ranges::sort(ranges);

        for (size_t i = 1; i < ranges.size(); ++i)
        {
            EXPECT_GE(ranges[i].first, ranges[i - 1].second)
                << "Overlap detected: entries at offsets "
                << ranges[i - 1].first << " and " << ranges[i].first;
        }

        // Verify total file size
        uint64_t total_size = 0;
        for (const auto sz : sizes)
        {
            total_size += sz;
        }
        EXPECT_EQ(df.Size(), total_size);

        co_return;
    }(ctx);

    ctx.RunUntilDone(std::move(test));
}

TEST_F(FileTest, MultipleDataFileInstances_CausesCorruption)
{
    // This test demonstrates WHY you should NEVER have multiple DataFile
    // instances pointing to the same file - they maintain independent
    // offset tracking and will write to overlapping positions.

    auto path = temp_dir / "corruption.db";
    const int fd1 = open(path.c_str(), config.write_flags, config.file_mode);
    ASSERT_GE(fd1, 0) << "Failed to create test file";
    const int fd2 = open(path.c_str(), config.write_flags, config.file_mode);
    ASSERT_GE(fd2, 0) << "Failed to open same file again";

    auto shared_fd1 = std::make_shared<kio::FD>(fd1);
    auto shared_fd2 = std::make_shared<kio::FD>(fd2);
    DataFile df1 = DataFile(shared_fd1, 1, config);
    DataFile df2 = DataFile(shared_fd2, 1, config);

    auto test = [&](kio::IoContext& io) -> kio::Task<>
    {
        std::string val1 = "first_value";
        std::string val2 = "second_value";

        DataEntry entry1("key1", std::as_bytes(std::span(val1)));
        DataEntry entry2("key2", std::as_bytes(std::span(val2)));

        // Both DataFile instances start with offset 0
        auto result1 = co_await df1.AsyncWrite(io, entry1);
        auto result2 = co_await df2.AsyncWrite(io, entry2);

        EXPECT_TRUE(result1.has_value());
        EXPECT_TRUE(result2.has_value());

        // Both return offset 0 - THIS IS THE BUG!
        EXPECT_EQ(result1.value(), 0);
        EXPECT_EQ(result2.value(), 0) << "Bug: Both writes claim offset 0!";

        // Both DataFile objects think they've written correctly
        EXPECT_EQ(df1.Size(), entry1.Size());
        EXPECT_EQ(df2.Size(), entry2.Size());

        // But the actual file is corrupted (second write overwrote the first)
        // We can verify by checking the actual file size
        struct stat st{};
        const int stat_result = fstat(fd1, &st);
        EXPECT_EQ(stat_result, 0) << "fstat failed";

        if (stat_result == 0)
        {
            // File size should be BOTH entries if no corruption, but it's only ONE
            const size_t expected_if_no_bug = entry1.Size() + entry2.Size();
            const size_t actual_size = st.st_size;

            EXPECT_NE(actual_size, expected_if_no_bug)
                << "If this passes, the bug is fixed (or writes are serialized somehow)";
        }

        co_return;
    }(ctx);

    ctx.RunUntilDone(std::move(test));
}

TEST_F(FileTest, ShouldRotate_TriggersWhenSizeExceeded)
{
    auto path = temp_dir / "rotate.db";
    const int fd = open(path.c_str(), config.write_flags, config.file_mode);
    ASSERT_GE(fd, 0) << "Failed to create test file";

    auto shared_fd = std::make_shared<kio::FD>(fd);
    DataFile df = DataFile(shared_fd, 1, config);
    const size_t max_size = 200; // Small limit for quick test

    auto test = [&](kio::IoContext& io) -> kio::Task<>
    {
        // Initially should not rotate
        EXPECT_FALSE(df.ShouldRotate(max_size));

        // Write a small entry - still under limit
        std::string val1 = "small";
        DataEntry entry1("key1", std::as_bytes(std::span(val1)));
        auto result1 = co_await df.AsyncWrite(io, entry1);
        EXPECT_TRUE(result1.has_value());
        EXPECT_FALSE(df.ShouldRotate(max_size)) << "Should not rotate yet";

        // Write a large entry that exceeds the limit
        std::string val2(max_size, 'X');
        DataEntry entry2("key2", std::as_bytes(std::span(val2)));
        auto result2 = co_await df.AsyncWrite(io, entry2);
        EXPECT_TRUE(result2.has_value());

        // Now should trigger rotation
        EXPECT_TRUE(df.ShouldRotate(max_size))
            << "Should rotate when size (" << df.Size() << ") >= max (" << max_size << ")";

        co_return;
    }(ctx);

    ctx.RunUntilDone(std::move(test));
}

TEST_F(FileTest, DataSurvivesClose)
{
    auto path = temp_dir / "persist.db";
    std::string test_key = "persistent_key";
    std::string test_val = "persistent_value";
    uint64_t written_offset = 0;
    size_t entry_size = 0;

    // Phase 1: Write and close
    {
        const int fd = open(path.c_str(), config.write_flags, config.file_mode);
        ASSERT_GE(fd, 0) << "Failed to create test file";

        auto shared_fd = std::make_shared<kio::FD>(fd);
        DataFile df = DataFile(shared_fd, 1, config);

        auto write_task = [&](kio::IoContext& io) -> kio::Task<>
        {
            DataEntry entry(test_key, std::as_bytes(std::span(test_val)));
            entry_size = entry.Size();

            auto result = co_await df.AsyncWrite(io, entry);
            EXPECT_TRUE(result.has_value());
            written_offset = result.value();
            EXPECT_EQ(df.Size(), entry_size);
            co_return;
        }(ctx);

        ctx.RunUntilDone(std::move(write_task));
    }

    // Phase 2: Reopen and verify data
    {
        auto read_task = [&](kio::IoContext& io) -> kio::Task<>
        {
            // Reopen for reading
            auto fd_result = co_await kio::AsyncOpen(io, path, O_RDONLY, 0);
            EXPECT_TRUE(fd_result.has_value());
            const int fd = fd_result.value().Get();

            // Read the entry back
            std::vector<std::byte> buffer(entry_size);
            auto read_result = co_await kio::AsyncReadExact(io, fd, buffer, written_offset);
            EXPECT_TRUE(read_result.has_value());

            // Deserialize and verify
            auto entry_result = DataEntry::Deserialize(buffer);
            EXPECT_TRUE(entry_result.has_value());

            const auto& recovered = entry_result.value();
            EXPECT_EQ(recovered.GetKeyView(), test_key) << "Key mismatch after reopen";

            // Convert value back to string for comparison
            auto recovered_val = recovered.GetValueView();
            const std::string recovered_str(
                reinterpret_cast<const char*>(recovered_val.data()),
                recovered_val.size()
            );
            EXPECT_EQ(recovered_str, test_val) << "Value mismatch after reopen";

            co_return;
        }(ctx);

        ctx.RunUntilDone(std::move(read_task));
    }
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
