//
// Created by Yao ACHI on 13/11/2025.
//

#include <filesystem>
#include <gtest/gtest.h>

#include "bitcask/include/data_file.h"
#include "bitcask/include/hint_file.h"
#include "core/include/io/worker.h"
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class DataFileTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;
    BitcaskConfig config_;

    void SetUp() override {
        alog::configure(1024, LogLevel::Disabled);
        // Create a temporary test directory
        test_dir_ = std::filesystem::temp_directory_path() / "bitcask_test";
        std::filesystem::create_directories(test_dir_);

        config_.directory = test_dir_;
        config_.max_file_size = 1024 * 1024; // 1MB
        config_.sync_on_write = false; // Speedup tests

        // Create worker
        WorkerConfig worker_config;
        worker_config.uring_queue_depth = 128;
        worker_ = std::make_unique<Worker>(0, worker_config);

        // Start a worker thread
        worker_thread_ = std::thread([this]() {
            worker_->loop_forever();
        });

        worker_->wait_ready();
    }

    void TearDown() override {
        if (worker_) {
            (void)worker_->request_stop();
            worker_->wait_shutdown();
        }

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        // Now safe to destroy worker_
        worker_.reset();

        // Cleanup test directory
        std::filesystem::remove_all(test_dir_);
    }

    // Helper to create a data file
    Task<Result<std::unique_ptr<DataFile>>> create_data_file(uint64_t file_id) {
        co_await SwitchToWorker(*worker_);

        const auto path = test_dir_ / std::format("data_{}.db", file_id);
        int fd = KIO_TRY(co_await worker_->async_openat(
            path,
            config_.write_flags,
            config_.file_mode
        ));

        auto file = std::make_unique<DataFile>(fd, file_id, *worker_, config_);
        co_return std::move(file);
    }

    // Helper to read file content
    [[nodiscard]] Task<Result<std::vector<char>>> read_file_content(const std::filesystem::path& path) const
    {
        co_await SwitchToWorker(*worker_);

        int fd = KIO_TRY(co_await worker_->async_openat(
            path,
            O_RDONLY,
            0644
        ));

        auto size = std::filesystem::file_size(path);
        std::vector<char> buffer(size);

        KIO_TRY(co_await worker_->async_read_exact_at(fd, buffer, 0));
        KIO_TRY(co_await worker_->async_close(fd));

        co_return buffer;
    }
};

TEST_F(DataFileTest, BasicWrite) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto file_result = co_await create_data_file(1);
        EXPECT_TRUE(file_result.has_value());
        const auto file = std::move(file_result.value());

        // Write entry
        const DataEntry entry("key1", std::vector{'v', 'a', 'l', '1'});
        auto write_result = co_await file->async_write(entry);

        EXPECT_TRUE(write_result.has_value());
        auto [offset, size] = write_result.value();

        EXPECT_EQ(offset, 0) << "First entry should be at offset 0";
        EXPECT_GT(size, 0) << "Entry size should be positive";
        EXPECT_EQ(file->size(), size);

        co_await file->async_close();
    };

    SyncWait(test_coro());
}

TEST_F(DataFileTest, MultipleWrites) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto file = std::move((co_await create_data_file(2)).value());

        std::vector<std::pair<uint64_t, uint32_t>> write_results;

        // Write 10 entries
        for (int i = 0; i < 10; ++i) {
            std::string key = std::format("key_{}", i);
            std::vector value = {'v', 'a', 'l', static_cast<char>('0' + i)};

            DataEntry entry(std::move(key), std::move(value));
            auto write_result = co_await file->async_write(entry);
            EXPECT_TRUE(write_result.has_value());

            auto [offset, size] = write_result.value();
            write_results.emplace_back(offset, size);
        }

        // Verify offsets are sequential
        uint64_t expected_offset = 0;
        for (const auto& [offset, size] : write_results) {
            EXPECT_EQ(offset, expected_offset);
            expected_offset += size;
        }

        EXPECT_EQ(file->size(), expected_offset);

        co_await file->async_close();
    };

    SyncWait(test_coro());
}

TEST_F(DataFileTest, ShouldRotate) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        const auto file = std::move((co_await create_data_file(3)).value());

        // Initially should not rotate
        EXPECT_FALSE(file->should_rotate(config_.max_file_size));

        // Write past the max size
        std::vector large_value(config_.max_file_size + 4, 'X');
        const DataEntry large_entry("large_key", std::move(large_value));

        const auto result = co_await file->async_write(large_entry);
        EXPECT_TRUE(result.has_value());

        // Now should rotate
        EXPECT_TRUE(file->should_rotate(config_.max_file_size));

        co_await file->async_close();
    };

    SyncWait(test_coro());
}

// Write Persistence (Data Survives Close)
TEST_F(DataFileTest, WritePersistence) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        uint64_t file_id = 4;
        auto file_path = test_dir_ / std::format("data_{}.db", file_id);

        // Write data
        {
            auto file = std::move((co_await create_data_file(file_id)).value());

            DataEntry entry("persistent_key", std::vector{'p', 'e', 'r', 's', 'i', 's', 't'});
            auto write_result = co_await file->async_write(entry);
            EXPECT_TRUE(write_result.has_value());

            auto [offset, size] = write_result.value();
            EXPECT_EQ(offset, 0);
            EXPECT_GT(size, 0);

            co_await file->async_close();
        }

        // Read back and verify
        auto content_result = co_await read_file_content(file_path);
        EXPECT_TRUE(content_result.has_value());
        auto content = content_result.value();

        EXPECT_FALSE(content.empty());

        // Deserialize and verify
        auto result = DataEntry::deserialize(content);
        EXPECT_TRUE(result.has_value());

        if (result.has_value()) {
            auto [recovered, _] = result.value();
            EXPECT_EQ(recovered.key, "persistent_key");
            EXPECT_EQ(recovered.value, std::vector({'p', 'e', 'r', 's', 'i', 's', 't'}));
        }
    };

    SyncWait(test_coro());
}

// ----------------------------------------------------------------------------
// Test 5: Tombstone Entry Write
// ----------------------------------------------------------------------------
TEST_F(DataFileTest, TombstoneWrite) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto file = std::move((co_await create_data_file(5)).value());

        DataEntry tombstone("deleted_key", std::vector<char>{}, kFlagTombstone);
        auto write_result = co_await file->async_write(tombstone);
        EXPECT_TRUE(write_result.has_value());

        auto [offset, size] = write_result.value();

        EXPECT_EQ(offset, 0);
        EXPECT_GT(size, 0);

        co_await file->async_close();

        // Verify tombstone flag persists
        auto file_path = test_dir_ / "data_5.db";
        auto content_result = co_await read_file_content(file_path);
        EXPECT_TRUE(content_result.has_value());

        if (content_result.has_value()) {
            auto content = content_result.value();
            auto result = DataEntry::deserialize(content);
            EXPECT_TRUE(result.has_value());

            if (result.has_value()) {
                auto [recovered, _] = result.value();
                EXPECT_TRUE(recovered.is_tombstone());
                EXPECT_EQ(recovered.key, "deleted_key");
            }
        }
    };

    SyncWait(test_coro());
}

// ============================================================================
// HintFile Tests
// ============================================================================

class HintFileTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;  // Use std::thread, not std::jthread
    BitcaskConfig config_;

    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "bitcask_hint_test";
        std::filesystem::create_directories(test_dir_);

        config_.directory = test_dir_;

        WorkerConfig worker_config;
        worker_ = std::make_unique<Worker>(0, worker_config);

        worker_thread_ = std::thread([this]() {
            worker_->loop_forever();
        });

        worker_->wait_ready();
    }

    void TearDown() override {
        if (worker_) {
            (void)worker_->request_stop();
            worker_->wait_shutdown();
        }

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        worker_.reset();

        std::filesystem::remove_all(test_dir_);
    }

    Task<Result<std::unique_ptr<HintFile>>> create_hint_file(uint64_t file_id) {
        co_await SwitchToWorker(*worker_);

        const auto path = test_dir_ / std::format("hint_{}.ht", file_id);
        int fd = KIO_TRY(co_await worker_->async_openat(
            path,
            config_.write_flags,
            config_.file_mode
        ));

        auto file = std::make_unique<HintFile>(fd, file_id, *worker_, config_);
        co_return std::move(file);
    }

    [[nodiscard]] Task<Result<std::vector<char>>> read_file_content(const std::filesystem::path& path) const
    {
        co_await SwitchToWorker(*worker_);

        const int fd = KIO_TRY(co_await worker_->async_openat(path, O_RDONLY, 0644));

        const auto size = std::filesystem::file_size(path);
        std::vector<char> buffer(size);

        KIO_TRY(co_await worker_->async_read_exact_at(fd, buffer, 0));
        KIO_TRY(co_await worker_->async_close(fd));

        co_return buffer;
    }
};

TEST_F(HintFileTest, BasicWrite) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto hint_file = std::move((co_await create_hint_file(1)).value());

        HintEntry hint(12345, 100, 50, "test_key");
        const auto result = co_await hint_file->async_write(std::move(hint));

        EXPECT_TRUE(result.has_value());

        // Verify file exists and has content
        const auto hint_path = test_dir_ / "hint_1.ht";
        EXPECT_TRUE(std::filesystem::exists(hint_path));
        EXPECT_GT(std::filesystem::file_size(hint_path), 0);
    };

    SyncWait(test_coro());
}

TEST_F(HintFileTest, MultipleHintEntries) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        auto hint_file = std::move((co_await create_hint_file(2)).value());

        std::vector<HintEntry> hints;
        hints.reserve(10);
        for (int i = 0; i < 10; ++i) {
            hints.emplace_back(
                get_current_timestamp(),
                i * 100,
                50 + i,
                std::format("key_{}", i)
            );
        }

        // Write all hints
        for (auto& hint : hints) {
            auto result = co_await hint_file->async_write(std::move(hint));
            EXPECT_TRUE(result.has_value());
        }

        // Read back and deserialize
        auto hint_path = test_dir_ / "hint_2.ht";
        auto content_result = co_await read_file_content(hint_path);
        EXPECT_TRUE(content_result.has_value());

        if (!content_result.has_value()) {
            co_return;
        }

        auto content = content_result.value();
        std::span<const char> buffer(content);
        int recovered_count = 0;

        while (!buffer.empty()) {
            auto result = HintEntry::deserialize(buffer);
            if (!result.has_value()) break;

            const auto& recovered = result.value();
            auto serialized_size = struct_pack::get_needed_size(recovered);

            EXPECT_EQ(recovered.key, std::format("key_{}", recovered_count));

            buffer = buffer.subspan(serialized_size);
            recovered_count++;
        }

        EXPECT_EQ(recovered_count, 10);
    };

    SyncWait(test_coro());
}

TEST_F(HintFileTest, PersistenceAndRecovery) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        uint64_t file_id = 3;
        auto hint_path = test_dir_ / std::format("hint_{}.ht", file_id);

        // Write hints
        {
            auto hint_file = std::move((co_await create_hint_file(file_id)).value());

            HintEntry hint1(1000, 0, 100, "key_a");
            HintEntry hint2(2000, 100, 200, "key_b");
            HintEntry hint3(3000, 300, 150, "key_c");

            co_await hint_file->async_write(std::move(hint1));
            co_await hint_file->async_write(std::move(hint2));
            co_await hint_file->async_write(std::move(hint3));
        }

        // Recover hints
        auto content_result = co_await read_file_content(hint_path);
        EXPECT_TRUE(content_result.has_value());

        if (!content_result.has_value()) {
            co_return;
        }

        auto content = content_result.value();
        std::span<const char> buffer(content);
        std::vector<HintEntry> recovered_hints;

        while (!buffer.empty()) {
            auto result = HintEntry::deserialize(buffer);
            if (!result.has_value()) break;

            auto hint = result.value();
            auto size = struct_pack::get_needed_size(hint);

            recovered_hints.push_back(std::move(hint));
            buffer = buffer.subspan(size);
        }

        EXPECT_EQ(recovered_hints.size(), 3);

        if (recovered_hints.size() >= 3) {
            EXPECT_EQ(recovered_hints[0].key, "key_a");
            EXPECT_EQ(recovered_hints[0].entry_pos, 0);
            EXPECT_EQ(recovered_hints[0].total_sz, 100);

            EXPECT_EQ(recovered_hints[1].key, "key_b");
            EXPECT_EQ(recovered_hints[1].entry_pos, 100);

            EXPECT_EQ(recovered_hints[2].key, "key_c");
            EXPECT_EQ(recovered_hints[2].entry_pos, 300);
        }
    };

    SyncWait(test_coro());
}

TEST_F(DataFileTest, DataAndHintCoordination) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        uint64_t file_id = 10;

        auto data_file = std::move((co_await create_data_file(file_id)).value());

        auto hint_path = test_dir_ / std::format("hint_{}.ht", file_id);
        auto res = co_await worker_->async_openat(
            hint_path,
            config_.write_flags,
            config_.file_mode
        );
        EXPECT_TRUE(res.has_value());
        int hint_fd = res.value();
        auto hint_file = std::make_unique<HintFile>(hint_fd, file_id, *worker_, config_);

        // Write coordinated entries
        std::vector<std::string> keys = {"apple", "banana", "cherry"};

        for (const auto& key : keys) {
            std::vector<char> value(key.begin(), key.end());
            DataEntry entry(std::string(key), std::move(value));

            auto write_result = co_await data_file->async_write(entry);
            EXPECT_TRUE(write_result.has_value());

            if (!write_result.has_value()) continue;

            auto [offset, size] = write_result.value();

            // Write corresponding hint
            HintEntry hint(
                get_current_timestamp(),
                offset,
                size,
                std::string(key)
            );
            co_await hint_file->async_write(std::move(hint));
        }

        co_await data_file->async_close();

        // Verify we can recover using hints
        auto hint_result = co_await read_file_content(hint_path);
        EXPECT_TRUE(hint_result.has_value());

        if (!hint_result.has_value()) {
            co_return;
        }

        auto hint_content = hint_result.value();
        std::span<const char> hint_buffer(hint_content);

        // Read the data file
        auto data_path = test_dir_ / std::format("data_{}.db", file_id);
        auto data_result = co_await read_file_content(data_path);
        EXPECT_TRUE(data_result.has_value());

        if (!data_result.has_value()) {
            co_return;
        }

        auto data_content = data_result.value();

        int verified_count = 0;
        while (!hint_buffer.empty()) {
            auto hint_res = HintEntry::deserialize(hint_buffer);
            if (!hint_res.has_value()) break;

            const auto& hint = hint_res.value();
            auto hint_size = struct_pack::get_needed_size(hint);

            // Use hint to locate data
            std::span<const char> data_at_offset(
                data_content.data() + hint.entry_pos,
                hint.total_sz
            );

            auto data_res = DataEntry::deserialize(data_at_offset);
            EXPECT_TRUE(data_res.has_value());

            if (data_res.has_value()) {
                auto [data_entry, _] = data_res.value();
                EXPECT_EQ(data_entry.key, hint.key);
            }

            hint_buffer = hint_buffer.subspan(hint_size);
            verified_count++;
        }

        EXPECT_EQ(verified_count, 3);
    };

    SyncWait(test_coro());
}

TEST_F(DataFileTest, SyncOnWrite) {
    auto test_coro = [&]() -> Task<void> {
        co_await SwitchToWorker(*worker_);

        // Enable sync_on_write
        config_.sync_on_write = true;

        const auto file = std::move((co_await create_data_file(11)).value());

        const DataEntry entry("sync_key", std::vector{'s', 'y', 'n', 'c'});

        const auto start = std::chrono::high_resolution_clock::now();
        const auto result = co_await file->async_write(entry);
        const auto end = std::chrono::high_resolution_clock::now();

        EXPECT_TRUE(result.has_value());

        // Sync should take measurable time (> 0ms)
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        EXPECT_GT(duration.count(), 0);

        co_await file->async_close();
    };

    SyncWait(test_coro());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}