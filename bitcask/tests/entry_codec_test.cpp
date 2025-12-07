#include <gtest/gtest.h>

#include "../../kio/sync/sync_wait.h"
#include "bitcask/include/entry.h"

using namespace bitcask;
using namespace kio;
using namespace kio::io;

class EntrySerializationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        alog::configure(1024, LogLevel::Disabled);
    }
};

TEST_F(EntrySerializationTest, DataEntryRoundTrip) {
    std::string key = "test_key";
    std::vector value = {'v', 'a', 'l', 'u', 'e'};

    DataEntry original{std::string(key), std::vector(value)};

    // Serialize
    auto serialized = original.serialize();
    ASSERT_GT(serialized.size(), kEntryFixedHeaderSize)
        << "Serialized size must include header";

    // Deserialize
    auto result = DataEntry::deserialize(serialized);
    ASSERT_TRUE(result.has_value()) << "Deserialization should succeed";

    auto [recovered, bytes_read] = result.value();

    // Verify
    EXPECT_EQ(recovered.key, original.key);
    EXPECT_EQ(recovered.value, original.value);
    EXPECT_EQ(recovered.flag, original.flag);
    EXPECT_EQ(recovered.timestamp_ns, original.timestamp_ns);
    EXPECT_EQ(bytes_read, serialized.size());
}

TEST_F(EntrySerializationTest, TombstoneEntry) {
    const DataEntry tombstone("deleted_key", std::vector<char>{}, kFlagTombstone);

    EXPECT_TRUE(tombstone.is_tombstone());

    auto serialized = tombstone.serialize();
    auto result = DataEntry::deserialize(serialized);

    ASSERT_TRUE(result.has_value());
    auto [recovered, _] = result.value();

    EXPECT_TRUE(recovered.is_tombstone());
    EXPECT_EQ(recovered.key, tombstone.key);
    EXPECT_TRUE(recovered.value.empty());
}

TEST_F(EntrySerializationTest, LargeValue) {
    std::string key = "large_key";
    std::vector large_value(10 * 1024 * 1024, 'X'); // 10MB

    const DataEntry entry{std::string(key), std::vector(large_value)};

    auto serialized = entry.serialize();
    auto result = DataEntry::deserialize(serialized);

    ASSERT_TRUE(result.has_value());
    auto [recovered, _] = result.value();

    EXPECT_EQ(recovered.value.size(), large_value.size());
    EXPECT_EQ(recovered.value, large_value);
}

TEST_F(EntrySerializationTest, EmptyKeyAndValue) {
    const DataEntry entry("", std::vector<char>{});

    auto serialized = entry.serialize();
    auto result = DataEntry::deserialize(serialized);

    ASSERT_TRUE(result.has_value());
    auto [recovered, _] = result.value();

    EXPECT_TRUE(recovered.key.empty());
    EXPECT_TRUE(recovered.value.empty());
}

TEST_F(EntrySerializationTest, CRCCorruptionDetection) {
    DataEntry entry("key", std::vector{'v', 'a', 'l'});
    auto serialized = entry.serialize();

    // Corrupt the CRC (first 4 bytes)
    serialized[0] ^= 0xFF;

    auto result = DataEntry::deserialize(serialized);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value, kIoDataCorrupted);
}

TEST_F(EntrySerializationTest, TruncatedData) {
    const DataEntry entry("key", std::vector<char>{'v', 'a', 'l', 'u', 'e'});
    auto serialized = entry.serialize();

    // Truncate the buffer
    std::span<const char> truncated(serialized.data(), kEntryFixedHeaderSize - 1);

    auto result = DataEntry::deserialize(truncated);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value, kIoNeedMoreData);
}

TEST_F(EntrySerializationTest, PartialEntryInBuffer) {
    DataEntry entry1("key1", std::vector{'a'});
    DataEntry entry2("key2", std::vector{'b'});

    auto s1 = entry1.serialize();
    auto s2 = entry2.serialize();

    // Create a buffer with entry1 + half of entry2
    std::vector<char> buffer;
    buffer.insert(buffer.end(), s1.begin(), s1.end());
    buffer.insert(buffer.end(), s2.begin(), s2.begin() + static_cast<int>(s2.size()) / 2);

    // Parse entry1
    auto result1 = DataEntry::deserialize(buffer);
    ASSERT_TRUE(result1.has_value());
    auto [e1, offset1] = result1.value();
    EXPECT_EQ(e1.key, "key1");

    // Try parse entry2 (should fail - need more data)
    std::span<const char> remaining(buffer.data() + offset1, buffer.size() - offset1);
    auto result2 = DataEntry::deserialize(remaining);
    EXPECT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error().value, kIoNeedMoreData);
}

TEST_F(EntrySerializationTest, MultipleEntriesInBuffer) {
    const std::vector entries = {
        DataEntry("key1", std::vector{'a'}),
        DataEntry("key2", std::vector{'b', 'c'}),
        DataEntry("key3", std::vector{'d', 'e', 'f'}),
    };

    // Serialize all
    std::vector<char> buffer;
    for (const auto& e : entries) {
        auto s = e.serialize();
        buffer.insert(buffer.end(), s.begin(), s.end());
    }

    // Deserialize all
    std::span<const char> remaining = buffer;
    for (size_t i = 0; i < entries.size(); ++i) {
        auto result = DataEntry::deserialize(remaining);
        ASSERT_TRUE(result.has_value()) << "Entry " << i << " should deserialize";

        auto [e, bytes] = result.value();
        EXPECT_EQ(e.key, entries[i].key);
        EXPECT_EQ(e.value, entries[i].value);

        remaining = remaining.subspan(bytes);
    }

    EXPECT_TRUE(remaining.empty()) << "All data should be consumed";
}

TEST_F(EntrySerializationTest, HintEntryRoundTrip) {
    const HintEntry hint(12345, 100, 50, "test_key");

    auto serialized = hint.serialize();
    auto result = HintEntry::deserialize(serialized);

    ASSERT_TRUE(result.has_value());
    const auto& recovered = result.value();

    EXPECT_EQ(recovered.timestamp_ns, hint.timestamp_ns);
    EXPECT_EQ(recovered.entry_pos, hint.entry_pos);
    EXPECT_EQ(recovered.total_sz, hint.total_sz);
    EXPECT_EQ(recovered.key, hint.key);
}

TEST_F(EntrySerializationTest, EndiannessConsistency) {
    const DataEntry entry("key", std::vector{'v', 'a', 'l'});
    const auto serialized = entry.serialize();

    // Read CRC and size as little-endian
    const auto crc = read_le<uint32_t>(serialized.data());
    const auto size = read_le<uint64_t>(serialized.data() + 4);

    EXPECT_GT(crc, 0u) << "CRC should be non-zero";
    EXPECT_GT(size, 0u) << "Size should be non-zero";
    EXPECT_EQ(serialized.size(), kEntryFixedHeaderSize + size);
}

TEST_F(EntrySerializationTest, BinarySafety) {
    std::vector<char> binary_value = {
        char(0x01), char(0x02), char(0x03),
        char(0x04), char(0xFF), char(0xFE)
    };;
    auto binary_key = std::string("\x00\xFF\x80", 3);

    const DataEntry entry{std::string(binary_key), std::vector<char>(binary_value)};

    auto serialized = entry.serialize();
    auto result = DataEntry::deserialize(serialized);

    ASSERT_TRUE(result.has_value());
    auto [recovered, _] = result.value();

    EXPECT_EQ(recovered.key, binary_key);
    EXPECT_EQ(recovered.value, binary_value);
}

TEST_F(EntrySerializationTest, DISABLE_SerializationPerformance) {
    constexpr int iterations = 100000;
    std::vector value(1024, 'X'); // 1KB

    const auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        DataEntry entry("key_" + std::to_string(i), std::vector(value));
        auto serialized = entry.serialize();
        auto result = DataEntry::deserialize(serialized);
        //ASSERT_TRUE(result.has_value());
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Serialization Performance: "
              << iterations << " round-trips in "
              << duration.count() << "ms ("
              << (iterations * 1000.0 / static_cast<double>(duration.count())) << " ops/sec)"
              << std::endl;
}

// ============================================================================
// File I/O Integration Tests
// ============================================================================

class EntryFileIOTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir_;
    std::unique_ptr<Worker> worker_;
    std::thread worker_thread_;

    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "bitcask_entry_file_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        WorkerConfig worker_config;
        worker_config.uring_queue_depth = 128;
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
};

TEST_F(EntryFileIOTest, WriteAndReadSingleEntry) {
    auto test_coro = [&]() -> Task<Result<void>> {
        co_await SwitchToWorker(*worker_);

        auto test_file = test_dir_ / "single_entry.db";

        // Write entry to file
        DataEntry original{"test_key", std::vector{'v', 'a', 'l', 'u', 'e'}};
        auto serialized = original.serialize();

        int write_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_CREAT | O_WRONLY | O_TRUNC, 0644));

        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, serialized, 0));
        KIO_TRY(co_await worker_->async_fsync(write_fd));
        KIO_TRY(co_await worker_->async_close(write_fd));

        // Read entry from file
        int read_fd = KIO_TRY(co_await worker_->async_openat(test_file, O_RDONLY, 0644));

        auto file_size = std::filesystem::file_size(test_file);
        std::vector<char> buffer(file_size);

        KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, 0));
        KIO_TRY(co_await worker_->async_close(read_fd));

        // Deserialize and verify
        auto result = DataEntry::deserialize(buffer);
        EXPECT_TRUE(result.has_value());

        auto [recovered, bytes_read] = result.value();
        EXPECT_EQ(recovered.key, original.key);
        EXPECT_EQ(recovered.value, original.value);
        EXPECT_EQ(bytes_read, serialized.size());

        co_return {};
    };

    //
    if (auto res = SyncWait(test_coro()); !res.has_value()) {
        ALOG_ERROR("Test function thrown an error: {}", res.error());
    }
}

TEST_F(EntryFileIOTest, WriteAndReadMultipleEntries) {
    auto test_coro = [&]() -> Task<Result<void>> {
        co_await SwitchToWorker(*worker_);

        auto test_file = test_dir_ / "multiple_entries.db";

        // Create multiple entries
        std::vector entries = {
            DataEntry{"key1", std::vector{'v', 'a', 'l', '1'}},
            DataEntry{"key2", std::vector{'v', 'a', 'l', 'u', 'e', '2'}},
            DataEntry{"key3", std::vector{'x', 'y', 'z'}},
            DataEntry{"longer_key_name", std::vector(100, 'A')},
        };

        // Serialize all entries into one buffer
        std::vector<char> combined_buffer;
        for (const auto& entry : entries) {
            auto serialized = entry.serialize();
            combined_buffer.insert(combined_buffer.end(), serialized.begin(), serialized.end());
        }

        // Write to file
        int write_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_CREAT | O_WRONLY | O_TRUNC, 0644));

        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, combined_buffer, 0));
        KIO_TRY(co_await worker_->async_fsync(write_fd));
        KIO_TRY(co_await worker_->async_close(write_fd));

        // Read from file
        int read_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_RDONLY, 0644));

        auto file_size = std::filesystem::file_size(test_file);
        std::vector<char> read_buffer(file_size);

        KIO_TRY(co_await worker_->async_read_exact_at(read_fd, read_buffer, 0));
        KIO_TRY(co_await worker_->async_close(read_fd));

        // Deserialize all entries
        std::span<const char> remaining = read_buffer;
        for (size_t i = 0; i < entries.size(); ++i) {
            auto result = DataEntry::deserialize(remaining);
            EXPECT_TRUE(result.has_value()) << "Entry " << i << " should deserialize";

            auto [recovered, bytes] = result.value();
            EXPECT_EQ(recovered.key, entries[i].key) << "Key mismatch at entry " << i;
            EXPECT_EQ(recovered.value, entries[i].value) << "Value mismatch at entry " << i;

            remaining = remaining.subspan(bytes);
        }

        EXPECT_TRUE(remaining.empty()) << "All data should be consumed";

        co_return {};
    };

    if (auto res = SyncWait(test_coro()); !res.has_value()) {
        ALOG_ERROR("Test function thrown an error: {}", res.error());
    }
}

TEST_F(EntryFileIOTest, WriteAtOffsetAndReadBack) {
    auto test_coro = [&]() -> Task<Result<void>> {
        co_await SwitchToWorker(*worker_);

        auto test_file = test_dir_ / "offset_entries.db";

        // Create entries
        DataEntry entry1{"first", std::vector{'1', '2', '3'}};
        DataEntry entry2{"second", std::vector{'4', '5', '6', '7'}};
        DataEntry entry3{"third", std::vector{'8', '9'}};

        auto s1 = entry1.serialize();
        auto s2 = entry2.serialize();
        auto s3 = entry3.serialize();

        // Write to file at specific offsets
        int write_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_CREAT | O_WRONLY | O_TRUNC, 0644));

        uint64_t offset = 0;
        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, s1, offset));
        offset += s1.size();

        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, s2, offset));
        offset += s2.size();

        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, s3, offset));

        KIO_TRY(co_await worker_->async_fsync(write_fd));
        KIO_TRY(co_await worker_->async_close(write_fd));

        // Read each entry at specific offset
        int read_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_RDONLY, 0644));

        // Read entry 1 at offset 0
        {
            std::vector<char> buffer(s1.size());
            KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, 0));

            auto result = DataEntry::deserialize(buffer);
            EXPECT_TRUE(result.has_value());
            auto [recovered, _] = result.value();
            EXPECT_EQ(recovered.key, "first");
            EXPECT_EQ(recovered.value, std::vector({'1', '2', '3'}));
        }

        // Read entry 2 at offset s1.size()
        {
            std::vector<char> buffer(s2.size());
            KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, s1.size()));

            auto result = DataEntry::deserialize(buffer);
            EXPECT_TRUE(result.has_value());
            auto [recovered, _] = result.value();
            EXPECT_EQ(recovered.key, "second");
            EXPECT_EQ(recovered.value, std::vector({'4', '5', '6', '7'}));
        }

        // Read entry 3 at offset s1.size() + s2.size()
        {
            std::vector<char> buffer(s3.size());
            KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, s1.size() + s2.size()));

            auto result = DataEntry::deserialize(buffer);
            EXPECT_TRUE(result.has_value());
            auto [recovered, _] = result.value();
            EXPECT_EQ(recovered.key, "third");
            EXPECT_EQ(recovered.value, std::vector({'8', '9'}));
        }

        KIO_TRY(co_await worker_->async_close(read_fd));

        co_return {};
    };

    if (auto res = SyncWait(test_coro()); !res.has_value()) {
        ALOG_ERROR("Test function thrown an error: {}", res.error());
    }
}

TEST_F(EntryFileIOTest, ReadPartialEntryDetection) {
    auto test_coro = [&]() -> Task<Result<void>> {
        co_await SwitchToWorker(*worker_);

        auto test_file = test_dir_ / "partial_entry.db";

        DataEntry entry{"test_key", std::vector(100, 'X')};
        auto serialized = entry.serialize();

        // Write full entry
        int write_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_CREAT | O_WRONLY | O_TRUNC, 0644));
        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, serialized, 0));
        KIO_TRY(co_await worker_->async_close(write_fd));

        // Read only partial data (header only)
        int read_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_RDONLY, 0644));

        std::vector<char> partial_buffer(kEntryFixedHeaderSize);
        KIO_TRY(co_await worker_->async_read_exact_at(read_fd, partial_buffer, 0));

        // Should fail with kIoNeedMoreData
        auto result = DataEntry::deserialize(partial_buffer);
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_EQ(result.error().value, kIoNeedMoreData);
        }

        KIO_TRY(co_await worker_->async_close(read_fd));

        co_return {};
    };

    if (auto res = SyncWait(test_coro()); !res.has_value()) {
        ALOG_ERROR("Test function thrown an error: {}", res.error());
    }
}

TEST_F(EntryFileIOTest, SimulatePartitionReadWrite) {
    auto test_coro = [&]() -> Task<Result<void>> {
        co_await SwitchToWorker(*worker_);

        auto test_file = test_dir_ / "partition_simulation.db";

        // This simulates what partition.cpp does:
        // 1. Write entry
        // 2. Track offset and size
        // 3. Read back using offset and size

        struct WriteResult {
            std::string key;
            uint64_t offset;
            uint32_t size;
        };

        std::vector<WriteResult> write_results;

        // Open file for writing
        int write_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_CREAT | O_WRONLY | O_TRUNC, 0644));

        uint64_t current_offset = 0;

        // Write multiple entries
        for (int i = 0; i < 10; ++i) {
            std::string key = std::format("partition_key_{}", i);
            std::string val_str = std::format("partition_value_{}", i);
            std::vector<char> value(val_str.begin(), val_str.end());

            DataEntry entry{std::string(key), std::move(value)};
            auto serialized = entry.serialize();

            KIO_TRY(co_await worker_->async_write_exact_at(write_fd, serialized, current_offset));

            write_results.push_back({key, current_offset, static_cast<uint32_t>(serialized.size())});
            current_offset += serialized.size();
        }

        KIO_TRY(co_await worker_->async_fdatasync(write_fd));
        KIO_TRY(co_await worker_->async_close(write_fd));

        // Open file for reading
        int read_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_RDONLY, 0644));

        // Read back each entry using offset and size (like partition.cpp does)
        for (const auto& wr : write_results) {
            std::vector<char> buffer(wr.size);
            KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, wr.offset));

            auto result = DataEntry::deserialize(buffer);
            EXPECT_TRUE(result.has_value()) << "Failed to deserialize entry at offset " << wr.offset;

            auto [recovered, bytes_read] = result.value();
            EXPECT_EQ(recovered.key, wr.key)
                << "Key mismatch at offset " << wr.offset;
            EXPECT_EQ(bytes_read, wr.size)
                << "Size mismatch at offset " << wr.offset;
        }

        KIO_TRY(co_await worker_->async_close(read_fd));

        co_return {};
    };

    if (auto res = SyncWait(test_coro()); !res.has_value()) {
        ALOG_ERROR("Test function thrown an error: {}", res.error());
    }
}

TEST_F(EntryFileIOTest, TombstoneFileIO) {
    auto test_coro = [&]() -> Task<Result<void>> {
        co_await SwitchToWorker(*worker_);

        auto test_file = test_dir_ / "tombstone_test.db";

        // Write a regular entry, then a tombstone
        DataEntry regular{"active_key", std::vector{'v', 'a', 'l'}};
        DataEntry tombstone{"deleted_key", std::vector<char>{}, kFlagTombstone};

        auto s1 = regular.serialize();
        auto s2 = tombstone.serialize();

        int write_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_CREAT | O_WRONLY | O_TRUNC, 0644));

        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, s1, 0));
        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, s2, s1.size()));
        KIO_TRY(co_await worker_->async_fsync(write_fd));
        KIO_TRY(co_await worker_->async_close(write_fd));

        // Read back
        int read_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_RDONLY, 0644));

        // Read regular entry
        {
            std::vector<char> buffer(s1.size());
            KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, 0));

            auto result = DataEntry::deserialize(buffer);
            EXPECT_TRUE(result.has_value());
            auto [recovered, _] = result.value();
            EXPECT_FALSE(recovered.is_tombstone());
            EXPECT_EQ(recovered.key, "active_key");
        }

        // Read tombstone
        {
            std::vector<char> buffer(s2.size());
            KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, s1.size()));

            auto result = DataEntry::deserialize(buffer);
            EXPECT_TRUE(result.has_value());
            auto [recovered, _] = result.value();
            EXPECT_TRUE(recovered.is_tombstone());
            EXPECT_EQ(recovered.key, "deleted_key");
            EXPECT_TRUE(recovered.value.empty());
        }

        KIO_TRY(co_await worker_->async_close(read_fd));

        co_return {};
    };

    if (auto res = SyncWait(test_coro()); !res.has_value()) {
        ALOG_ERROR("Test function thrown an error: {}", res.error());
    }
}

TEST_F(EntryFileIOTest, WriteAppendReadConcurrent) {
    auto test_coro = [&]() -> Task<Result<void>> {
        co_await SwitchToWorker(*worker_);

        auto test_file = test_dir_ / "append_read_test.db";

        // Open file with O_APPEND for writing (simulating active file writes)
        int write_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_CREAT | O_WRONLY | O_APPEND, 0644));

        // Create and serialize multiple entries
        std::vector entries = {
            DataEntry{"key1", std::vector{'v', 'a', 'l', '1'}},
            DataEntry{"key2", std::vector{'v', 'a', 'l', 'u', 'e', '2'}},
            DataEntry{"key3", std::vector{'x', 'y', 'z'}},
        };

        std::vector<std::vector<char>> serialized_entries;
        std::vector<uint64_t> offsets;
        uint64_t current_offset = 0;

        // Write entries using O_APPEND
        for (const auto& entry : entries) {
            auto serialized = entry.serialize();

            // Record offset before write
            offsets.push_back(current_offset);

            // Write with O_APPEND (doesn't use offset parameter)
            KIO_TRY(co_await worker_->async_write_exact_at(write_fd, serialized, 0));

            serialized_entries.push_back(std::move(serialized));
            current_offset += serialized_entries.back().size();
        }

        // Flush to ensure data is visible to readers
        KIO_TRY(co_await worker_->async_fdatasync(write_fd));

        // DO NOT close write_fd yet - keep it open (simulating active file)

        // Now open the SAME file with O_RDONLY to read
        // This simulates what partition.cpp does when reading from an active file
        int read_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_RDONLY, 0644));

        // Read each entry using the recorded offsets
        for (size_t i = 0; i < entries.size(); ++i) {
            std::vector<char> buffer(serialized_entries[i].size());

            // Read at specific offset
            KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, offsets[i]));

            // Deserialize and verify
            auto result = DataEntry::deserialize(buffer);
            EXPECT_TRUE(result.has_value())
                << "Failed to deserialize entry " << i
                << " at offset " << offsets[i];

            auto [recovered, bytes_read] = result.value();
            EXPECT_EQ(recovered.key, entries[i].key)
                << "Key mismatch at entry " << i;
            EXPECT_EQ(recovered.value, entries[i].value)
                << "Value mismatch at entry " << i;
            EXPECT_EQ(bytes_read, serialized_entries[i].size())
                << "Size mismatch at entry " << i;
        }

        // Close read fd first
        KIO_TRY(co_await worker_->async_close(read_fd));

        // Write one more entry after closing the read fd
        DataEntry entry4{"key4", std::vector{'a', 'b', 'c', 'd'}};
        auto s4 = entry4.serialize();
        offsets.push_back(current_offset);

        KIO_TRY(co_await worker_->async_write_exact_at(write_fd, s4, 0));
        KIO_TRY(co_await worker_->async_fdatasync(write_fd));

        // Now close the write fd
        KIO_TRY(co_await worker_->async_close(write_fd));

        // Reopen as read-only and verify all entries including the last one
        read_fd = KIO_TRY(co_await worker_->async_openat(
            test_file, O_RDONLY, 0644));

        // Verify the last entry
        {
            std::vector<char> buffer(s4.size());
            KIO_TRY(co_await worker_->async_read_exact_at(read_fd, buffer, offsets[3]));

            auto result = DataEntry::deserialize(buffer);
            EXPECT_TRUE(result.has_value());

            auto [recovered, _] = result.value();
            EXPECT_EQ(recovered.key, "key4");
            EXPECT_EQ(recovered.value, std::vector({'a', 'b', 'c', 'd'}));
        }

        KIO_TRY(co_await worker_->async_close(read_fd));

        co_return {};
    };

    if (auto res = SyncWait(test_coro()); !res.has_value()) {
        ALOG_ERROR("Test function thrown an error: {}", res.error());
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}