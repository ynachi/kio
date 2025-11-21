//
// Created by Yao ACHI on 14/11/2025.
//
#include "bitcask/include/hint_file.h"

#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <span>
#include <thread>
#include <unistd.h>
#include <ylt/struct_pack.hpp>

#include "bitcask/include/config.h"
#include "bitcask/include/entry.h"
#include "bitcask/include/keydir.h"
#include "core/include/coro.h"
#include "core/include/ds/buffer_pool.h"
#include "core/include/errors.h"
#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;

class HintFileTest : public ::testing::Test
{
protected:
    BitcaskConfig config_;
    BufferPool buffer_pool_;
    io::WorkerConfig worker_config_;
    std::unique_ptr<io::Worker> worker_;
    std::jthread worker_thread_;

    int test_fd_ = -1;
    static constexpr uint64_t TEST_FILE_ID = 456;

    void SetUp() override
    {
        // Setup worker, following the pattern in datafile_test.cpp
        worker_config_.uring_submit_timeout_ms = 10;
        worker_ = std::make_unique<io::Worker>(0, worker_config_);
        worker_thread_ = std::jthread([this] { worker_->loop_forever(); });
        worker_->wait_ready();

        // --- Create a portable temporary file ---
        const std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        std::string temp_path_str = (temp_dir / "hintfile_test_XXXXXX").string();

        // Create and open the file
        test_fd_ = mkstemp(&temp_path_str[0]);
        ASSERT_GE(test_fd_, 0) << "Failed to create temp file: " << strerror(errno);

        // Unlink the file immediately for auto-cleanup
        ASSERT_EQ(::unlink(temp_path_str.c_str()), 0) << "Failed to unlink temp file";

        // --- Add O_APPEND flag ---
        // async_write implies O_APPEND, so we must add it.
        const int current_flags = fcntl(test_fd_, F_GETFL);
        ASSERT_NE(current_flags, -1) << "fcntl(F_GETFL) failed: " << strerror(errno);
        const int new_flags = current_flags | O_APPEND;
        ASSERT_NE(fcntl(test_fd_, F_SETFL, new_flags), -1) << "fcntl(F_SETFL, O_APPEND) failed: " << strerror(errno);
    }

    void TearDown() override
    {
        (void) worker_->request_stop();

        if (test_fd_ >= 0)
        {
            ::close(test_fd_);
        }
    }

    // Helper to create a test HintEntry
    static HintEntry create_hint_entry(std::string key, uint64_t ts, uint64_t pos, uint32_t sz)
    {
        // Use the constructor from entry.h
        return {ts, pos, sz, std::move(key)};
    }
};

TEST_F(HintFileTest, WriteAndReadRoundtrip)
{
    HintFile hf(test_fd_, TEST_FILE_ID, *worker_, config_);
    // HintFile now owns the fd
    test_fd_ = -1;

    KeyDir keydir(1);

    // Async write takes ownership (move), so create copies
    HintEntry entry1 = create_hint_entry("key1", 1000, 10, 150);
    HintEntry entry2 = create_hint_entry("key2", 2000, 160, 75);
    // Newer version of key1
    HintEntry entry3 = create_hint_entry("key1", 3000, 235, 150);

    auto test_task = [&]() -> Task<void>
    {
        // We MUST switch to the worker thread
        co_await io::SwitchToWorker(*worker_);

        // --- Write Entries ---
        // We move copies, as required by the async_write signature
        const auto write_res1 = co_await hf.async_write(create_hint_entry("key1", 1000, 10, 150));
        const auto write_res2 = co_await hf.async_write(create_hint_entry("key2", 2000, 160, 75));
        const auto write_res3 = co_await hf.async_write(create_hint_entry("key1", 3000, 235, 150));

        EXPECT_TRUE(write_res1.has_value());
        EXPECT_TRUE(write_res2.has_value());
        EXPECT_TRUE(write_res3.has_value());

        // --- Read and Populate KeyDir ---
        const auto read_res = co_await hf.async_read(keydir);
        EXPECT_TRUE(read_res.has_value());

        co_return;
    };

    // Run the entire test task
    SyncWait(test_task());

    // --- Verify KeyDir ---
    // After SyncWait returns, the KeyDir should be populated.

    // Check key1 (should have the *newest* value from entry3)
    auto loc1 = keydir.get("key1");
    EXPECT_TRUE(loc1.has_value());
    EXPECT_EQ(loc1->file_id, TEST_FILE_ID);
    EXPECT_EQ(loc1->timestamp_ns, entry3.timestamp_ns);
    EXPECT_EQ(loc1->offset, entry3.entry_pos);
    EXPECT_EQ(loc1->total_size, entry3.total_sz);

    // Check key2
    auto loc2 = keydir.get("key2");
    EXPECT_TRUE(loc2.has_value());
    EXPECT_EQ(loc2->file_id, TEST_FILE_ID);
    EXPECT_EQ(loc2->timestamp_ns, entry2.timestamp_ns);
    EXPECT_EQ(loc2->offset, entry2.entry_pos);
    EXPECT_EQ(loc2->total_size, entry2.total_sz);

    // Check non-existent key
    auto loc_bad = keydir.get("bad_key");
    EXPECT_FALSE(loc_bad.has_value());
}

TEST_F(HintFileTest, ReadEmptyFile)
{
    const HintFile hf(test_fd_, TEST_FILE_ID, *worker_, config_);
    test_fd_ = -1;  // HintFile now owns the fd

    KeyDir keydir(1);

    auto test_task = [&]() -> Task<void>
    {
        co_await io::SwitchToWorker(*worker_);
        const auto read_res = co_await hf.async_read(keydir);
        // Reading an empty file is not an error
        EXPECT_TRUE(read_res.has_value());
        co_return;
    };

    SyncWait(test_task());

    // KeyDir should be empty
    const auto snapshot = keydir.snapshot();
    EXPECT_TRUE(snapshot.empty());
}

TEST_F(HintFileTest, ReadCorruptedFile)
{
    // This test writes valid hint entries, then appends garbage bytes.
    // The read loop should process the valid entries and then fail
    // when it tries to deserialize the garbage.
    const HintFile hf(test_fd_, TEST_FILE_ID, *worker_, config_);
    test_fd_ = -1;

    KeyDir keydir(1);

    auto entry1 = create_hint_entry("good_key", 1000, 10, 150);

    auto test_task = [&]() -> Task<Result<void>>
    {
        co_await io::SwitchToWorker(*worker_);

        // Write one valid entry
        const auto write_res1 = co_await hf.async_write(std::move(entry1));
        EXPECT_TRUE(write_res1.has_value());

        // Write garbage data
        std::vector garbage{static_cast<char>(0xDE), static_cast<char>(0xAD), static_cast<char>(0xBE), static_cast<char>(0xEF)};
        const auto write_garbage_res = co_await worker_->async_write_exact(hf.fd(), std::span<const char>(garbage));
        EXPECT_TRUE(write_garbage_res.has_value()) << "Failed to write garbage data";

        // Now, try to read. This should read the first entry successfully,
        // then fail when it hits the garbage.
        co_return co_await hf.async_read(keydir);
    };

    // Run the task
    auto final_result = SyncWait(test_task());

    // The final read operation *should have failed*
    EXPECT_FALSE(final_result.has_value());

    // Even though the read failed, the KeyDir *should*
    // contain the valid entries it read *before* the corruption.
    const auto loc1 = keydir.get("good_key");
    EXPECT_TRUE(loc1.has_value());
    EXPECT_EQ(loc1->timestamp_ns, 1000);
    EXPECT_EQ(loc1->offset, 10);
}
