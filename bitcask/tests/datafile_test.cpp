//
// Created by Yao ACHI on 13/11/2025.
//

#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

#include "bitcask/include/data_file.h"
#include "bitcask/include/entry.h"
#include "core/include/coro.h"
#include "core/include/ds/buffer_pool.h"
#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;

class DataFileTest : public ::testing::Test
{
protected:
    BitcaskConfig config_;
    BufferPool buffer_pool_;
    io::WorkerConfig worker_config_;
    std::unique_ptr<io::Worker> worker_;
    std::jthread worker_thread_;

    int test_fd_ = -1;
    static constexpr uint64_t TEST_FILE_ID = 123;

    void SetUp() override
    {
        // Setup worker, following the pattern in worker_test.cpp
        worker_config_.uring_submit_timeout_ms = 10;
        worker_ = std::make_unique<io::Worker>(0, worker_config_);
        worker_thread_ = std::jthread([this] { worker_->loop_forever(); });
        worker_->wait_ready();

        // --- Create a portable temporary file ---
        const std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        std::string temp_path_str = (temp_dir / "datafile_test_XXXXXX").string();

        // Create and open the file. mkstemp modifies the string!
        test_fd_ = mkstemp(&temp_path_str[0]);
        ASSERT_GE(test_fd_, 0) << "Failed to create temp file: " << strerror(errno);

        // Unlink the file immediately.
        // The kernel keeps the file alive because our fd is open.
        // It's guaranteed to be cleaned up when the fd is closed or the test crashes.
        ASSERT_EQ(::unlink(temp_path_str.c_str()), 0) << "Failed to unlink temp file";

        // apply O_APPEND flag
        const int current_flags = fcntl(test_fd_, F_GETFL);
        ASSERT_NE(current_flags, -1) << "fcntl(F_GETFL) failed: " << strerror(errno);
        const int new_flags = current_flags | O_APPEND;
        ASSERT_NE(fcntl(test_fd_, F_SETFL, new_flags), -1) << "fcntl(F_SETFL, O_APPEND) failed: " << strerror(errno);
    }

    void TearDown() override
    {
        (void) worker_->request_stop();
        worker_thread_.join();

        if (test_fd_ >= 0)
        {
            ::close(test_fd_);
        }
    }

    // Helper to create a simple test entry
    static DataEntry create_test_entry(const std::string& key, const std::string& value) { return {std::string(key), {value.begin(), value.end()}}; }
};


TEST_F(DataFileTest, ConstructorAndClose)
{
    // The fd is now owned by DataFile, so we set ours to -1
    DataFile df(test_fd_, TEST_FILE_ID, *worker_, config_);
    test_fd_ = -1;

    EXPECT_EQ(df.file_id(), TEST_FILE_ID);
    EXPECT_EQ(df.size(), 0);

    auto close_task = [&]() -> Task<void>
    {
        // We must run async_close on the worker thread
        co_await io::SwitchToWorker(*worker_);
        const auto close_res = co_await df.async_close();
        EXPECT_TRUE(close_res.has_value());
        co_return;
    };

    SyncWait(close_task());
}

TEST_F(DataFileTest, WriteAndReadRoundtrip)
{
    config_.sync_on_write = false;
    DataFile df(test_fd_, TEST_FILE_ID, *worker_, config_);
    test_fd_ = -1;

    const DataEntry entry1 = create_test_entry("key1", "value1");
    const DataEntry entry2 = create_test_entry("key2", "value_two");

    const auto ser1 = entry1.serialize();
    const auto ser2 = entry2.serialize();

    auto test_task = [&]() -> Task<void>
    {
        // We MUST switch to the worker thread before calling DataFile methods
        co_await io::SwitchToWorker(*worker_);

        // --- Write first entry ---
        auto write_res1 = co_await df.async_write(entry1);
        EXPECT_TRUE(write_res1.has_value());
        auto res1 = write_res1.value();
        EXPECT_EQ(res1.first, 0);
        EXPECT_EQ(df.size(), ser1.size());

        // --- Write second entry ---
        auto write_res2 = co_await df.async_write(entry2);
        EXPECT_TRUE(write_res2.has_value());
        auto res2 = write_res2.value();
        EXPECT_EQ(res2.first, ser1.size());
        EXPECT_EQ(df.size(), ser1.size() + ser2.size());

        // --- Close ---
        auto close_res = co_await df.async_close();
        EXPECT_TRUE(close_res.has_value());
        co_return;
    };

    // Run the entire test task
    SyncWait(test_task());
}

TEST_F(DataFileTest, ShouldRotate)
{
    config_.max_file_size = 100;  // 100 bytes
    DataFile df(test_fd_, TEST_FILE_ID, *worker_, config_);
    test_fd_ = -1;

    auto test_task = [&]() -> Task<void>
    {
        co_await io::SwitchToWorker(*worker_);

        // File size is 0
        EXPECT_FALSE(df.should_rotate(config_.max_file_size));

        DataEntry entry1 = create_test_entry("key", "value");  // size > 12
        auto write_res1 = co_await df.async_write(entry1);
        EXPECT_TRUE(write_res1.has_value());
        EXPECT_GT(df.size(), 0);
        EXPECT_LT(df.size(), config_.max_file_size);
        EXPECT_FALSE(df.should_rotate(config_.max_file_size));

        // Write a large entry
        std::string large_val(100, 'x');
        DataEntry entry2 = create_test_entry("large", large_val);
        auto write_res2 = co_await df.async_write(entry2);
        EXPECT_TRUE(write_res2.has_value());

        // Now the size should be > 100
        EXPECT_GT(df.size(), config_.max_file_size);
        EXPECT_TRUE(df.should_rotate(config_.max_file_size));

        auto close_res = co_await df.async_close();
        EXPECT_TRUE(close_res.has_value());

        co_return;
    };

    SyncWait(test_task());
}
