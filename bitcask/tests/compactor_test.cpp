#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <ranges>
#include <thread>

#include "bitcask/include/compactor_old.h"
#include "core/include/coro.h"
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class CompactorTest : public ::testing::Test
{
protected:
    std::filesystem::path test_dir_;
    BitcaskConfig config_;
    std::unique_ptr<SimpleKeydir> keydir_;
    // Worker Components
    WorkerConfig worker_config_;
    std::unique_ptr<Worker> worker_;
    std::jthread worker_thread_;

    void SetUp() override
    {
        // Setup test directory
        test_dir_ = std::filesystem::temp_directory_path() / ("bitcask_test_" + std::to_string(time(nullptr)));
        std::filesystem::create_directories(test_dir_);

        config_.directory = test_dir_;
        config_.max_file_size = 10 * 1024;  // 10KB for testing
        config_.write_buffer_size = 1024;  // 1KB batches
        config_.read_buffer_size = 512;  // 512B reads

        keydir_ = std::make_unique<SimpleKeydir>(2);

        // Setup Worker Thread
        worker_config_.uring_submit_timeout_ms = 10;
        worker_ = std::make_unique<Worker>(0, worker_config_);
        worker_thread_ = std::jthread([this] { worker_->loop_forever(); });
        worker_->wait_ready();
    }

    void TearDown() override
    {
        if (worker_) (void) worker_->request_stop();

        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    // Helper: Create a test data file
    [[nodiscard]] Task<Result<uint64_t>> create_test_data_file(const std::vector<std::pair<std::string, std::string>>& entries) const
    {
        const uint64_t file_id = get_current_timestamp();
        const auto path = test_dir_ / std::format("data_{}.db", file_id);

        // Ensure we are on the worker thread
        co_await SwitchToWorker(*worker_);

        const int fd = KIO_TRY(co_await worker_->async_openat(path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        uint64_t offset = 0;
        for (auto& [key, value]: entries)
        {
            DataEntry entry;
            entry.timestamp_ns = get_current_timestamp();
            entry.key = key;
            entry.value.assign(value.begin(), value.end());

            auto serialized = entry.serialize();
            KIO_TRY(co_await worker_->async_write_exact(fd, serialized));

            // Update KeyDir (thread-safe)
            keydir_->insert_or_assign(entry.key, ValueLocation{file_id, offset, static_cast<uint32_t>(serialized.size()), entry.timestamp_ns});

            offset += serialized.size();
        }
        co_await worker_->async_close(fd);
        co_return file_id;
    }
};

// ============================================================================
// Basic Functionality Tests
// ============================================================================

TEST_F(CompactorTest, CompactEmptyFile)
{
    // Pass worker_ to Compactor
    Compactor compactor(*worker_, config_, *keydir_);

    auto test = [&]() -> Task<Result<void>>
    {
        co_await SwitchToWorker(*worker_);

        // Create an empty file
        uint64_t src_id = get_current_timestamp();
        const auto path = test_dir_ / std::format("data_{}.db", src_id);
        int fd = KIO_TRY(co_await worker_->async_openat(path, O_CREAT | O_WRONLY, 0644));
        co_await worker_->async_close(fd);

        // Create destination
        uint64_t dst_id = get_current_timestamp() + 1;
        const auto dst_path = test_dir_ / std::format("data_{}.db", dst_id);
        int dst_fd = KIO_TRY(co_await worker_->async_openat(dst_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        const auto dst_hint_path = test_dir_ / std::format("hint_{}.ht", dst_id);
        int dst_hint_fd = KIO_TRY(co_await worker_->async_openat(dst_hint_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        // Compact
        compactor.reset_for_new_merge();
        auto stats_result = co_await compactor.compact_one(src_id, dst_fd, dst_hint_fd, dst_id);

        EXPECT_TRUE(stats_result.has_value());
        const auto& stats = stats_result.value();

        EXPECT_EQ(stats.entries_kept, 0);
        EXPECT_EQ(stats.entries_discarded, 0);
        EXPECT_EQ(stats.bytes_read, 0);
        EXPECT_EQ(stats.bytes_written, 0);

        co_await worker_->async_close(dst_fd);
        co_await worker_->async_close(dst_hint_fd);

        // Verify source deleted
        EXPECT_FALSE(std::filesystem::exists(path));
        co_return {};
    };

    SyncWait(test());
}

TEST_F(CompactorTest, CompactAllLiveEntries)
{
    Compactor compactor(*worker_, config_, *keydir_);

    auto test = [&]() -> Task<Result<void>>
    {
        co_await SwitchToWorker(*worker_);

        std::vector<std::pair<std::string, std::string>> entries = {{"key1", "value1"}, {"key2", "value2"}, {"key3", "value3"}};
        uint64_t src_id = KIO_TRY(co_await create_test_data_file(entries));

        // Destination
        uint64_t dst_id = get_current_timestamp() + 1000;
        const auto dst_path = test_dir_ / std::format("data_{}.db", dst_id);
        int dst_fd = KIO_TRY(co_await worker_->async_openat(dst_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        const auto dst_hint_path = test_dir_ / std::format("hint_{}.ht", dst_id);
        int dst_hint_fd = KIO_TRY(co_await worker_->async_openat(dst_hint_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        compactor.reset_for_new_merge();
        auto stats_result = co_await compactor.compact_one(src_id, dst_fd, dst_hint_fd, dst_id);

        EXPECT_TRUE(stats_result.has_value());
        const auto& stats = stats_result.value();

        EXPECT_EQ(stats.entries_kept, 3);
        EXPECT_EQ(stats.entries_discarded, 0);
        EXPECT_GT(stats.bytes_read, 0);
        EXPECT_GT(stats.bytes_written, 0);

        co_await worker_->async_close(dst_fd);
        co_await worker_->async_close(dst_hint_fd);

        // Verify KeyDir updated
        for (const auto& key: entries | std::views::keys)
        {
            auto it = keydir_->find(key);
            EXPECT_TRUE(it != keydir_->end());
            auto loc = it->second;
            EXPECT_EQ(loc.file_id, dst_id);
        }

        co_return {};
    };

    SyncWait(test());
}

TEST_F(CompactorTest, CompactDiscardsStaleEntries)
{
    Compactor compactor(*worker_, config_, *keydir_);

    auto test = [&]() -> Task<Result<void>>
    {
        co_await SwitchToWorker(*worker_);

        // Create a source file
        std::vector<std::pair<std::string, std::string>> entries = {{"key1", "old_value"}, {"key2", "value2"}, {"key3", "old_value"}};
        uint64_t src_id = KIO_TRY(co_await create_test_data_file(entries));

        // Simulate newer updates in a separate file (making old entries stale)
        std::vector<std::pair<std::string, std::string>> new_entries = {{"key1", "new_value"}, {"key3", "new_value"}};
        uint64_t new_file_id = KIO_TRY(co_await create_test_data_file(new_entries));

        // Destination
        uint64_t dst_id = get_current_timestamp() + 2000;
        const auto dst_path = test_dir_ / std::format("data_{}.db", dst_id);
        int dst_fd = KIO_TRY(co_await worker_->async_openat(dst_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        const auto dst_hint_path = test_dir_ / std::format("hint_{}.ht", dst_id);
        int dst_hint_fd = KIO_TRY(co_await worker_->async_openat(dst_hint_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        compactor.reset_for_new_merge();
        auto stats_result = co_await compactor.compact_one(src_id, dst_fd, dst_hint_fd, dst_id);

        EXPECT_TRUE(stats_result.has_value());
        const auto& stats = stats_result.value();

        // Only key2 should be kept (key1 and key3 are stale)
        EXPECT_EQ(stats.entries_kept, 1);
        EXPECT_EQ(stats.entries_discarded, 2);

        co_await worker_->async_close(dst_fd);
        co_await worker_->async_close(dst_hint_fd);

        // Verify KeyDir logic
        auto it1 = keydir_->find("key1");
        EXPECT_TRUE(it1 != keydir_->end());
        auto loc1 = it1->second;
        EXPECT_EQ(loc1.file_id, new_file_id);

        auto it2 = keydir_->find("key2");
        EXPECT_TRUE(it2 != keydir_->end());
        auto loc2 = it2->second;
        EXPECT_EQ(loc2.file_id, dst_id);

        auto it3 = keydir_->find("key3");
        EXPECT_TRUE(it1 != keydir_->end());
        auto loc3 = it3->second;
        EXPECT_EQ(loc3.file_id, new_file_id);

        co_return {};
    };

    SyncWait(test());
}
// ============================================================================
// CAS (Race Condition) Tests
// ============================================================================

TEST_F(CompactorTest, CASFailureHandlesConcurrentUpdate)
{
    Compactor compactor(*worker_, config_, *keydir_);

    auto test = [&]() -> Task<Result<void>>
    {
        co_await SwitchToWorker(*worker_);

        std::vector<std::pair<std::string, std::string>> entries = {{"key1", "value1"}};
        uint64_t src_id = KIO_TRY(co_await create_test_data_file(entries));

        // Destination
        uint64_t dst_id = get_current_timestamp() + 1000;
        const auto dst_path = test_dir_ / std::format("data_{}.db", dst_id);
        int dst_fd = KIO_TRY(co_await worker_->async_openat(dst_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        const auto dst_hint_path = test_dir_ / std::format("hint_{}.ht", dst_id);
        int dst_hint_fd = KIO_TRY(co_await worker_->async_openat(dst_hint_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        compactor.reset_for_new_merge();

        // SIMULATE: User updates key1 WHILE we're compacting (updates KeyDir)
        uint64_t racing_file_id = get_current_timestamp() + 500;
        // Use assignment to overwrite the existing key!
        (*keydir_)["key1"] = {racing_file_id, 0, 100, get_current_timestamp()};

        auto stats_result = co_await compactor.compact_one(src_id, dst_fd, dst_hint_fd, dst_id);

        EXPECT_TRUE(stats_result.has_value());
        const auto& stats = stats_result.value();

        // Entry should be discarded because KeyDir changed from what we read at start of compaction
        EXPECT_EQ(stats.entries_kept, 0);
        EXPECT_EQ(stats.entries_discarded, 1);

        co_await worker_->async_close(dst_fd);
        co_await worker_->async_close(dst_hint_fd);

        // Verify KeyDir still points to racing update
        auto it = keydir_->find("key1");
        EXPECT_TRUE(it != keydir_->end());
        auto loc = it->second;
        EXPECT_EQ(loc.file_id, racing_file_id);

        co_return {};
    };

    SyncWait(test());
}

// ============================================================================
// N-to-1 Merge Tests
// ============================================================================

TEST_F(CompactorTest, NToOneMerge)
{
    Compactor compactor(*worker_, config_, *keydir_);

    auto test = [&]() -> Task<Result<void>>
    {
        co_await SwitchToWorker(*worker_);
        // Create 3 source files
        std::vector<uint64_t> src_ids;

        for (int i = 0; i < 3; i++)
        {
            std::vector<std::pair<std::string, std::string>> entries = {{std::format("key{}_1", i), std::format("value{}_1", i)}, {std::format("key{}_2", i), std::format("value{}_2", i)}};

            uint64_t id = KIO_TRY(co_await create_test_data_file(entries));
            src_ids.push_back(id);
        }

        // Create a single destination
        uint64_t dst_id = get_current_timestamp() + 10000;
        const auto dst_path = test_dir_ / std::format("data_{}.db", dst_id);
        int dst_fd = KIO_TRY(co_await worker_->async_openat(dst_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        const auto dst_hint_path = test_dir_ / std::format("hint_{}.ht", dst_id);
        int dst_hint_fd = KIO_TRY(co_await worker_->async_openat(dst_hint_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        // Compact all 3 files into 1
        compactor.reset_for_new_merge();
        Compactor::Stats total_stats;

        for (auto src_id: src_ids)
        {
            auto stats_result = co_await compactor.compact_one(src_id, dst_fd, dst_hint_fd, dst_id);

            EXPECT_TRUE(stats_result.has_value());
            auto& stats = stats_result.value();

            total_stats.bytes_read += stats.bytes_read;
            total_stats.bytes_written += stats.bytes_written;
            total_stats.entries_kept += stats.entries_kept;
            total_stats.entries_discarded += stats.entries_discarded;
            total_stats.files_compacted += stats.files_compacted;
        }

        co_await worker_->async_close(dst_fd);
        co_await worker_->async_close(dst_hint_fd);

        // Verify
        EXPECT_EQ(total_stats.files_compacted, 3);
        EXPECT_EQ(total_stats.entries_kept, 6);  // 3 files × 2 entries
        EXPECT_EQ(total_stats.entries_discarded, 0);

        // Verify all keys point to dst_id
        for (int i = 0; i < 3; i++)
        {
            for (int j = 1; j <= 2; j++)
            {
                auto key = std::format("key{}_{}", i, j);
                auto it = keydir_->find(key);
                EXPECT_TRUE(it != keydir_->end());
                auto loc = it->second;
                EXPECT_EQ(loc.file_id, dst_id);
            }
        }

        // Verify source files deleted
        for (auto src_id: src_ids)
        {
            auto path = test_dir_ / std::format("data_{}.db", src_id);
            EXPECT_FALSE(std::filesystem::exists(path));
        }

        co_return {};
    };

    SyncWait(test());
}

// ============================================================================
// Edge case
// ============================================================================

TEST_F(CompactorTest, CorruptFileDetection)
{
    Compactor compactor(*worker_, config_, *keydir_);

    auto test = [&]() -> Task<Result<void>>
    {
        co_await SwitchToWorker(*worker_);

        uint64_t src_id = get_current_timestamp();
        const auto path = test_dir_ / std::format("data_{}.db", src_id);
        int fd = KIO_TRY(co_await worker_->async_openat(path, O_CREAT | O_WRONLY, 0644));

        // Write bad data (header too short)
        std::vector<char> bad_data(8, 0);
        co_await worker_->async_write_exact(fd, bad_data);
        co_await worker_->async_close(fd);

        uint64_t dst_id = get_current_timestamp() + 1000;
        const auto dst_path = test_dir_ / std::format("data_{}.db", dst_id);
        int dst_fd = KIO_TRY(co_await worker_->async_openat(dst_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        const auto dst_hint_path = test_dir_ / std::format("hint_{}.ht", dst_id);
        int dst_hint_fd = KIO_TRY(co_await worker_->async_openat(dst_hint_path, O_CREAT | O_WRONLY | O_APPEND, 0644));

        compactor.reset_for_new_merge();
        auto stats_result = co_await compactor.compact_one(src_id, dst_fd, dst_hint_fd, dst_id);

        // Should fail
        EXPECT_FALSE(stats_result.has_value());
        EXPECT_EQ(stats_result.error().category, ErrorCategory::File);

        co_await worker_->async_close(dst_fd);
        co_await worker_->async_close(dst_hint_fd);

        co_return {};
    };

    SyncWait(test());
}
