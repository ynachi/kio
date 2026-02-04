//
// Created by Yao ACHI on 09/01/2026.
//

//
// Unit tests for DataFile
//
// Test the DataFile component including
// - Coroutine race condition fix (space reservation before yield)
// - O_APPEND rejection at construction time
// - File rotation logic
// - Write offset tracking
//

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <deque>

#include "bitcask/include/data_file.h"
#include "bitcask/include/entry.h"
#include "kio/core/worker.h"
#include "kio/sync/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class DataFileTest : public ::testing::Test
{
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;
    BitcaskConfig config_;

    void SetUp() override
    {
        alog::Configure(1024, LogLevel::kDisabled);

        test_dir_ = std::filesystem::temp_directory_path() / "datafile_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        config_.directory = test_dir_;
        config_.max_file_size = 1024 * 1024;  // 1MB
        config_.sync_on_write = false;
        config_.write_flags = O_CREAT | O_WRONLY;

        WorkerConfig worker_config;
        worker_config.uring_queue_depth = 128;
        worker_ = std::make_unique<Worker>(0, worker_config);

        worker_thread_ = std::thread([this]() { worker_->LoopForever(); });
        worker_->WaitReady();
    }

    void TearDown() override
    {
        if (worker_)
        {
            (void)worker_->RequestStop();
            worker_->WaitShutdown();
        }

        if (worker_thread_.joinable())
        {
            worker_thread_.join();
        }

        worker_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    // Helper to create a DataFile
    Task<Result<std::unique_ptr<DataFile>>> CreateDataFile(uint64_t file_id)
    {
        co_await SwitchToWorker(*worker_);

        auto path = test_dir_ / std::format("data_{}.db", file_id);
        int fd = KIO_TRY(co_await worker_->AsyncOpenAt(path, config_.write_flags, config_.file_mode));

        auto file = std::make_unique<DataFile>(fd, file_id, *worker_, config_);
        co_return std::move(file);
    }
};

// ============================================================================
// O_APPEND Rejection (Critical - Prevents Data Corruption)
// ============================================================================

TEST_F(DataFileTest, RejectsOAppendFd)
{
    // Opening with O_APPEND breaks pwrite() offset semantics
    const auto path = test_dir_ / "append_test.db";
    const int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    ASSERT_GE(fd, 0) << "Failed to create test file";

    EXPECT_THROW(
        DataFile(fd, 1, *worker_, config_),
        std::invalid_argument
    ) << "DataFile should reject O_APPEND file descriptors";

    close(fd);
}

TEST_F(DataFileTest, AcceptsNonAppendFd)
{
    auto path = test_dir_ / "normal_test.db";
    int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0644);
    ASSERT_GE(fd, 0);

    EXPECT_NO_THROW(DataFile(fd, 1, *worker_, config_));

    close(fd);
}

// ============================================================================
// Basic Write Operations
// ============================================================================

TEST_F(DataFileTest, BasicWrite)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto file = std::move((co_await CreateDataFile(1)).value());

        const DataEntry entry{"key1", "val1"};
        const auto result = co_await file->AsyncWrite(entry);

        EXPECT_TRUE(result.has_value());
        const uint64_t offset = result.value();

        EXPECT_EQ(offset, 0) << "First entry should be at offset 0";
        EXPECT_EQ(file->Size(), entry.Size());

        co_await file->AsyncClose();
    };

    SyncWait(test());
}

TEST_F(DataFileTest, SequentialWritesHaveCorrectOffsets)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto file = std::move((co_await CreateDataFile(2)).value());

        std::vector<uint64_t> offsets;
        std::vector<size_t> sizes;

        for (int i = 0; i < 10; ++i)
        {
            DataEntry entry{std::format("key_{}", i), "val"};
            sizes.push_back(entry.Size());

            auto result = co_await file->AsyncWrite(entry);
            EXPECT_TRUE(result.has_value());
            offsets.push_back(result.value());
        }

        // Verify offsets are sequential
        uint64_t expected_offset = 0;
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            EXPECT_EQ(offsets[i], expected_offset) << "Entry " << i << " has wrong offset";
            expected_offset += sizes[i];
        }

        EXPECT_EQ(file->Size(), expected_offset);

        co_await file->AsyncClose();
    };

    SyncWait(test());
}

// ============================================================================
// Coroutine Race Condition (Critical Bug Fix)
// ============================================================================

TEST_F(DataFileTest, ConcurrentCoroutineWritesNoOverlap)
{
    //
    // This tests the race condition fix in DataFile::AsyncWrite.
    //
    // Before the fix:
    //   1. Coroutine A reads size_ = 0
    //   2. Coroutine A yields on co_await
    //   3. Coroutine B reads size_ = 0 (still!)
    //   4. Both write to offset 0 → DATA CORRUPTION
    //
    // After the fix:
    //   1. Coroutine A reads size_ = 0, sets size_ = 100
    //   2. Coroutine A yields
    //   3. Coroutine B reads size_ = 100, sets size_ = 200
    //   4. A writes to 0, B writes to 100 → CORRECT
    //

    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto file = std::move((co_await CreateDataFile(3)).value());

        constexpr int kNumConcurrent = 50;
        std::vector<Task<Result<uint64_t>>> tasks;
        std::vector<size_t> entry_sizes;

        // CRITICAL FIX: The DataEntries passed to AsyncWrite must survive
        // until the async operation completes! Storing them in a deque/vector
        // ensures pointers in iovec (inside AsyncWrite) remain valid.
        std::deque<DataEntry> entries;

        // Launch many concurrent writes
        for (int i = 0; i < kNumConcurrent; ++i)
        {
            std::vector<char> value(100, 'X');
            // Emplace into deque to keep objects alive
            entries.emplace_back(std::format("concurrent_key_{:03}", i), value);

            entry_sizes.push_back(entries.back().Size());
            tasks.push_back(file->AsyncWrite(entries.back()));
        }

        // Collect all offsets
        std::vector<uint64_t> offsets;
        for (auto& task : tasks)
        {
            auto result = co_await std::move(task);
            EXPECT_TRUE(result.has_value());
            offsets.push_back(result.value());
        }

        // Verify NO overlapping regions
        // Sort offsets and check each entry's range doesn't overlap
        std::vector<std::pair<uint64_t, uint64_t>> ranges;  // (start, end)
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            ranges.emplace_back(offsets[i], offsets[i] + entry_sizes[i]);
        }

        std::ranges::sort(ranges);

        for (size_t i = 1; i < ranges.size(); ++i)
        {
            EXPECT_GE(ranges[i].first, ranges[i - 1].second)
                << "Overlap detected between entries at offsets "
                << ranges[i - 1].first << " and " << ranges[i].first;
        }

        // Verify total file size accounts for all entries
        uint64_t total_size = 0;
        for (size_t const sz : entry_sizes)
        {
            total_size += sz;
        }
        EXPECT_EQ(file->Size(), total_size);

        co_await file->AsyncClose();
    };

    SyncWait(test());
}

// ============================================================================
// File Rotation
// ============================================================================

TEST_F(DataFileTest, ShouldRotateWhenSizeExceeded)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        config_.max_file_size = 1024;  // 1KB
        auto file = std::move((co_await CreateDataFile(4)).value());

        EXPECT_FALSE(file->ShouldRotate(config_.max_file_size));

        // Write more than max_file_size
        std::vector large_value(config_.max_file_size + 100, 'X');
        const DataEntry large_entry{"large_key", std::move(large_value)};

        auto result = co_await file->AsyncWrite(large_entry);
        EXPECT_TRUE(result.has_value());

        EXPECT_TRUE(file->ShouldRotate(config_.max_file_size));

        co_await file->AsyncClose();
    };

    SyncWait(test());
}

// ============================================================================
// Data Persistence
// ============================================================================

TEST_F(DataFileTest, DataSurvivesClose)
{
    auto test = [&]() -> Task<Result<void>>
    {
        co_await SwitchToWorker(*worker_);

        uint64_t file_id = 5;
        auto file_path = test_dir_ / std::format("data_{}.db", file_id);

        // Explicit vector to ensure no hidden null terminators in span construction
        std::vector value_data{'p', 'e', 'r', 's', 'i', 's', 't'};

        // Write data
        {
            auto file = std::move((co_await CreateDataFile(file_id)).value());

            DataEntry const entry{"persistent_key", value_data};
            auto result = co_await file->AsyncWrite(entry);
            EXPECT_TRUE(result.has_value());

            co_await file->AsyncClose();
        }

        // Read back and verify
        int fd = KIO_TRY(co_await worker_->AsyncOpenAt(file_path, O_RDONLY, 0644));

        auto file_size = std::filesystem::file_size(file_path);
        std::vector<char> buffer(file_size);

        KIO_TRY(co_await worker_->AsyncReadExactAt(fd, buffer, 0));
        KIO_TRY(co_await worker_->AsyncClose(fd));

        auto result = DataEntry::Deserialize(buffer);
        EXPECT_TRUE(result.has_value());

        const auto& recovered = result.value();
        EXPECT_EQ(recovered.GetKeyView(), "persistent_key");
        auto recoved_value = std::vector(recovered.GetValueView().begin(), recovered.GetValueView().end());
        EXPECT_EQ(recoved_value, value_data);

        co_return {};
    };

    SyncWait(test());
}

// ============================================================================
// Scatter-Gather Write (Vectored I/O)
// ============================================================================

TEST_F(DataFileTest, ScatterGatherWriteMatchesEntryWrite)
{
    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        auto file1 = std::move((co_await CreateDataFile(6)).value());
        auto file2 = std::move((co_await CreateDataFile(7)).value());

        std::string key = "test_key";
        std::vector<char> value = {'t', 'e', 's', 't', '_', 'v', 'a', 'l', 'u', 'e'};
        uint64_t timestamp = GetCurrentTimestamp();

        // Write using DataEntry
        DataEntry entry{std::string(key), std::vector<char>(value), 0, timestamp};
        auto result1 = co_await file1->AsyncWrite(entry);

        // Write using scatter-gather
        auto result2 = co_await file2->AsyncWrite(key, value, timestamp, 0);

        EXPECT_TRUE(result1.has_value());
        EXPECT_TRUE(result2.has_value());

        // Both should produce same offset (0)
        EXPECT_EQ(result1.value(), result2.value());

        // Both files should have same size
        EXPECT_EQ(file1->Size(), file2->Size());

        co_await file1->AsyncClose();
        co_await file2->AsyncClose();
    };

    SyncWait(test());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}