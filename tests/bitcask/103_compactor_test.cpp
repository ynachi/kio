#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <string>
#include <random>

#include "kio/kio.hpp"
#include "bitcask/compactor.hpp"
#include "bitcask/files.hpp"
#include "bitcask/entry.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

// TODO: compaction code moved to Partition, move the tests there
class CompactorTest : public ::testing::Test
{
protected:
    fs::path temp_dir;
    kio::IoContext ctx;
    BitcaskConfig config;
    PartitionStats stats;
    std::unique_ptr<PartitionIO> io;
    std::unique_ptr<Compactor> compactor;

    void SetUp() override
    {
        temp_dir = fs::temp_directory_path() / ("bitcask_compact_test_" + std::to_string(getpid()));
        fs::create_directories(temp_dir);
        config.directory = temp_dir;
        // We need to create the partition directory structure expected by CompactFiles
        //  constructs a path: cfg.directory / "partition_{id}" / "data_{id}.db"
        fs::create_directories(temp_dir / "partition_1");

        io = std::make_unique<PartitionIO>(config, 1, stats);
        compactor = std::make_unique<Compactor>(*io, config, stats);
    }

    void TearDown() override
    {
        // Clean up FDs before deleting files
        if (io)
        {
            io->GetFDCache().Clear();
        }
        compactor.reset();
        io.reset();
        fs::remove_all(temp_dir);
    }

    // Helper to generate a random string of a specific size
    std::string RandomString(size_t length)
    {
        static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::string result;
        result.resize(length);
        for (size_t i = 0; i < length; i++)
        {
            result[i] = charset[rand() % (sizeof(charset) - 1)];
        }
        return result;
    }

    // Helper to create a file and write entries
    // Returns a map of Key -> ValueLocation (as if they were all live initially)
    kio::Task<std::vector<std::pair<std::string, ValueLocation>>>
    CreateDataFile(uint64_t file_id, const std::vector<std::pair<std::string, std::string>>& entries)
    {
        auto path = io->GetDataFilePath(file_id);

        int fd = open(path.c_str(), config.write_flags, config.file_mode);
        if (fd < 0) throw std::runtime_error("Failed to open file");

        auto shared_fd = std::make_shared<kio::FD>(fd);
        DataFile df(shared_fd, file_id, config);
        std::vector<std::pair<std::string, ValueLocation>> locations;

        for (const auto& [key, val] : entries)
        {
            DataEntry entry(key, std::as_bytes(std::span(val)));

            // Write
            auto res = co_await df.AsyncWrite(ctx, entry);
            if (!res) throw std::runtime_error("Failed to write entry");

            const uint64_t offset = res.value();

            locations.push_back({
                key, ValueLocation{
                    .file_id = file_id,
                    .offset = offset,
                    .total_size = entry.Size(),
                    .timestamp_ns = entry.GetTimestamp()
                }
            });
        }

        // Sync data to disk BEFORE closing
        // Without this, the data may still be in kernel buffers when we reopen the file
        auto sync_result = co_await kio::AsyncFdatasync(ctx, shared_fd->Get());
        if (!sync_result)
        {
            throw std::runtime_error("Failed to sync file");
        }

        co_return locations;
    }

    // Helper to read all contents of a file to verify
    kio::Task<std::vector<DataEntry>> ReadAllEntries(uint64_t file_id)
    {
        auto path = io->GetDataFilePath(file_id);

        if (!fs::exists(path) || fs::file_size(path) == 0)
        {
            co_return {};
        }

        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
        {
            ALOG_ERROR("Failed to open file {} for reading: {}", file_id, strerror(errno));
            co_return {};
        }

        auto file_size = fs::file_size(path);
        std::vector<std::byte> buffer(file_size);
        auto res = co_await kio::AsyncReadExact(ctx, fd, buffer, 0);
        close(fd);

        if (!res)
        {
            ALOG_ERROR("Failed to read file {}: {}", file_id, res.error().message());
            co_return {};
        }

        std::vector<DataEntry> entries;
        size_t offset = 0;
        std::span<const std::byte> view = buffer;

        while (offset < file_size)
        {
            // Need at least a header
            if (view.size() < kEntryFixedHeaderSize) break;

            // Deserialize peeks sizes first
            auto entry_res = DataEntry::Deserialize(view);
            if (!entry_res) break;

            entries.push_back(std::move(entry_res.value()));

            size_t size = entries.back().Size();
            offset += size;
            view = view.subspan(size);
        }

        co_return entries;
    }
};

TEST_F(CompactorTest, AllLiveNoReclaim)
{
    bool test_passed = false;
    std::string failure_message;

    auto task = [&]() -> kio::Task<>
    {
        std::vector<std::pair<std::string, std::string>> data = {
            {"key1", "val1"},
            {"key2", "val2"},
            {"key3", "long_value_string"}
        };

        auto locs = co_await CreateDataFile(100, data);

        auto& key_dir = io->GetKeyDir();
        for (const auto& [k, loc] : locs)
        {
            key_dir[k] = loc;
        }

        auto dst_path = io->GetDataFilePath(200);
        kio::Result<CompactionResult> result_ex;
        {
            int dst_fd = open(dst_path.c_str(), config.write_flags, config.file_mode);
            if (dst_fd < 0)
            {
                failure_message = "Failed to open destination file";
                co_return;
            }
            auto shared_fd = std::make_shared<kio::FD>(dst_fd);
            DataFile dst_file(shared_fd, 200, config);

            result_ex = co_await compactor->CompactFiles(ctx, {100}, dst_file);
        }
        if (!result_ex.has_value())
        {
            failure_message = "CompactFiles failed: " + result_ex.error().message();
            co_return;
        }

        auto result = result_ex.value();

        if (result.bytes_reclaimed != 0)
        {
            failure_message = std::format("Expected bytes_reclaimed=0, got {}", result.bytes_reclaimed);
            co_return;
        }

        if (result.new_hints.size() != 3)
        {
            failure_message = std::format("Expected 3 hints, got {}", result.new_hints.size());
            co_return;
        }

        if (result.new_hints[0].key != "key1")
        {
            failure_message = std::format("Expected first hint key='key1', got '{}'", result.new_hints[0].key);
            co_return;
        }

        if (result.new_hints[0].offset != 0)
        {
            failure_message = std::format("Expected first hint offset=0, got {}", result.new_hints[0].offset);
            co_return;
        }

        if (result.new_hints[1].key != "key2")
        {
            failure_message = std::format("Expected second hint key='key2', got '{}'", result.new_hints[1].key);
            co_return;
        }

        if (result.new_hints[1].offset != result.new_hints[0].size)
        {
            failure_message = std::format("Expected second hint offset={}, got {}",
                                          result.new_hints[0].size, result.new_hints[1].offset);
            co_return;
        }

        auto new_entries = co_await ReadAllEntries(200);

        if (new_entries.size() != 3)
        {
            failure_message = std::format("Expected 3 entries in compacted file, got {}", new_entries.size());
            co_return;
        }

        if (new_entries[0].GetKeyView() != "key1")
        {
            failure_message = std::format("Expected first entry key='key1', got '{}'",
                                          std::string(new_entries[0].GetKeyView()));
            co_return;
        }

        test_passed = true;
    };

    ctx.RunUntilDone(task());

    EXPECT_TRUE(test_passed) << failure_message;
}

TEST_F(CompactorTest, MixedLiveAndStaleReclaimsSpace)
{
    bool test_passed = false;
    std::string failure_message;

    auto task = [&]() -> kio::Task<>
    {
        std::vector<std::pair<std::string, std::string>> data = {
            {"key1", "keep_me"},
            {"key2", "stale_data"},
            {"key3", "deleted_data"}
        };
        auto locs = co_await CreateDataFile(100, data);

        auto& key_dir = io->GetKeyDir();
        key_dir["key1"] = locs[0].second;

        // Key2 is stale (points to file 999)
        ValueLocation newer_loc = locs[1].second;
        newer_loc.file_id = 999;
        key_dir["key2"] = newer_loc;

        // Key3 is missing (deleted) -> implicit

        // Prepare Dest
        auto dst_path = io->GetDataFilePath(200);
        kio::Result<CompactionResult> result_opt;
        {
            int dst_fd = open(dst_path.c_str(), config.write_flags, config.file_mode);
            if (dst_fd < 0)
            {
                failure_message = "Failed to open destination file";
                co_return;
            }
            auto shared_fd = std::make_shared<kio::FD>(dst_fd);
            DataFile dst_file(shared_fd, 200, config);

            // Compact
            result_opt = co_await compactor->CompactFiles(ctx, {100}, dst_file);
        }
        if (!result_opt.has_value())
        {
            failure_message = "CompactFiles failed: " + result_opt.error().message();
            co_return;
        }

        auto result = result_opt.value();

        // Verify: Should only have 1 hint (Key1)
        if (result.new_hints.size() != 1)
        {
            failure_message = std::format("Expected 1 hint, got {}", result.new_hints.size());
            co_return;
        }

        if (result.new_hints[0].key != "key1")
        {
            failure_message = std::format("Expected hint key='key1', got '{}'", result.new_hints[0].key);
            co_return;
        }

        // Reclaimed bytes should be size of Key2 + Key3
        uint64_t expected_reclaim = locs[1].second.total_size + locs[2].second.total_size;
        if (result.bytes_reclaimed != expected_reclaim)
        {
            failure_message = std::format("Expected bytes_reclaimed={}, got {}",
                                          expected_reclaim, result.bytes_reclaimed);
            co_return;
        }

        // Verify a file only has 1 entry
        auto new_entries = co_await ReadAllEntries(200);
        if (new_entries.size() != 1)
        {
            failure_message = std::format("Expected 1 entry in compacted file, got {}", new_entries.size());
            co_return;
        }

        if (new_entries[0].GetKeyView() != "key1")
        {
            failure_message = std::format("Expected entry key='key1', got '{}'",
                                          std::string(new_entries[0].GetKeyView()));
            co_return;
        }

        test_passed = true;
    };

    ctx.RunUntilDone(task());
    EXPECT_TRUE(test_passed) << failure_message;
}

TEST_F(CompactorTest, MultipleSourceFilesMerge)
{
    bool test_passed = false;
    std::string failure_message;

    auto task = [&]() -> kio::Task<>
    {
        // Create File 100
        auto locs1 = co_await CreateDataFile(100, {
                                                 {"KeyA", "v1"},
                                                 {"KeyB", "v1"}
                                             });

        // Create File 101
        auto locs2 = co_await CreateDataFile(101, {
                                                 {"KeyA", "v2"}
                                             });

        auto& key_dir = io->GetKeyDir();
        key_dir["KeyB"] = locs1[1].second; // KeyB from file 100 is live
        key_dir["KeyA"] = locs2[0].second; // KeyA from file 101 is live (v1 is stale)

        // Dest
        auto dst_path = io->GetDataFilePath(200);
        kio::Result<CompactionResult> result_opt;
        {
            int dst_fd = open(dst_path.c_str(), config.write_flags, config.file_mode);
            if (dst_fd < 0)
            {
                failure_message = "Failed to open destination file";
                co_return;
            }
            auto shared_fd = std::make_shared<kio::FD>(dst_fd);
            DataFile dst_file(shared_fd, 200, config);

            // Compact {100, 101}
            result_opt = co_await compactor->CompactFiles(ctx, {100, 101}, dst_file);
        }
        if (!result_opt.has_value())
        {
            failure_message = "CompactFiles failed: " + result_opt.error().message();
            co_return;
        }

        auto result = result_opt.value();

        if (result.new_hints.size() != 2)
        {
            failure_message = std::format("Expected 2 hints, got {}", result.new_hints.size());
            co_return;
        }

        // The order depends on the iteration order of src_ids and data in files
        // We expect KeyB (from 100) then KeyA (from 101)
        if (result.new_hints[0].key != "KeyB")
        {
            failure_message = std::format("Expected first hint key='KeyB', got '{}'", result.new_hints[0].key);
            co_return;
        }

        if (result.new_hints[1].key != "KeyA")
        {
            failure_message = std::format("Expected second hint key='KeyA', got '{}'", result.new_hints[1].key);
            co_return;
        }

        if (result.bytes_reclaimed != locs1[0].second.total_size)
        {
            failure_message = std::format("Expected bytes_reclaimed={}, got {}",
                                          locs1[0].second.total_size, result.bytes_reclaimed);
            co_return;
        }

        auto entries = co_await ReadAllEntries(200);
        if (entries.size() != 2)
        {
            failure_message = std::format("Expected 2 entries, got {}", entries.size());
            co_return;
        }

        // Verify values
        std::string val0(reinterpret_cast<const char*>(entries[0].GetValueView().data()),
                         entries[0].GetValueView().size());
        if (val0 != "v1")
        {
            failure_message = std::format("Expected first entry value='v1', got '{}'", val0);
            co_return;
        }

        std::string val1(reinterpret_cast<const char*>(entries[1].GetValueView().data()),
                         entries[1].GetValueView().size());
        if (val1 != "v2")
        {
            failure_message = std::format("Expected second entry value='v2', got '{}'", val1);
            co_return;
        }

        test_passed = true;
    };

    ctx.RunUntilDone(task());
    EXPECT_TRUE(test_passed) << failure_message;
}

TEST_F(CompactorTest, TruncatedEntryIgnored)
{
    bool test_passed = false;
    std::string failure_message;

    auto task = [&]() -> kio::Task<>
    {
        // Write a valid file
        const auto locs = co_await CreateDataFile(100, {
                                                      {"key1", "val1"},
                                                      {"key2", "val2"}
                                                  });

        // Manually truncate the file to cut off half of key2
        auto path = io->GetDataFilePath(100);
        uint64_t valid_size = locs[0].second.total_size;
        uint64_t partial_size = valid_size + 10; // 10 bytes of the next header
        fs::resize_file(path, partial_size);

        auto& key_dir = io->GetKeyDir();
        key_dir["key1"] = locs[0].second;
        // Key2 might be in KeyDir pointing to this file, but since file is physically truncated,
        // it effectively doesn't exist.
        key_dir["key2"] = locs[1].second;

        auto dst_path = io->GetDataFilePath(200);
        kio::Result<CompactionResult> result_opt;
        {
            int dst_fd = open(dst_path.c_str(), config.write_flags, config.file_mode);
            if (dst_fd < 0)
            {
                failure_message = "Failed to open destination file";
                co_return;
            }
            auto shared_fd = std::make_shared<kio::FD>(dst_fd);
            DataFile dst_file(shared_fd, 200, config);

            // Run Compactor
            result_opt = co_await compactor->CompactFiles(ctx, {100}, dst_file);
        }
        if (!result_opt.has_value())
        {
            failure_message = "CompactFiles failed: " + result_opt.error().message();
            co_return;
        }

        auto result = result_opt.value();

        if (result.new_hints.size() < 1)
        {
            failure_message = std::format("Expected at least 1 hint, got {}", result.new_hints.size());
            co_return;
        }

        if (result.new_hints[0].key != "key1")
        {
            failure_message = std::format("Expected first hint key='key1', got '{}'", result.new_hints[0].key);
            co_return;
        }

        if (result.new_hints.size() > 1)
        {
            failure_message = "Should not have recovered key2 from truncated data";
            co_return;
        }

        test_passed = true;
    };

    ctx.RunUntilDone(task());
    EXPECT_TRUE(test_passed) << failure_message;
}

TEST_F(CompactorTest, AllEntriesStaleFileDeleted)
{
    // Scenario: File 100 has keys, but KeyDir says they are all in File 101 or deleted.
    // Result: File 200 (compaction output) should be empty/0 bytes.
    bool passed = false;
    std::string failure_message;

    auto task = [&]() -> kio::Task<>
    {
        const auto locs = co_await CreateDataFile(100, {{"k1", "v1"}, {"k2", "v2"}});

        io->GetKeyDir().clear(); // Empty keydir implies keys are deleted or exist elsewhere

        auto dst_path = io->GetDataFilePath(200);
        kio::Result<CompactionResult> result_opt;
        {
            int dst_fd = open(dst_path.c_str(), config.write_flags, config.file_mode);
            if (dst_fd < 0)
            {
                failure_message = "Failed to open destination file";
                co_return;
            }
            auto shared_fd = std::make_shared<kio::FD>(dst_fd);
            DataFile dst_file(shared_fd, 200, config);

            result_opt = co_await compactor->CompactFiles(ctx, {100}, dst_file);
        }

        if (!result_opt.has_value())
        {
            failure_message = "Compaction failed: " + result_opt.error().message();
            co_return;
        }

        auto result = result_opt.value();

        if (result.new_hints.size() != 0)
        {
            failure_message = std::format("Expected 0 hints, got {}", result.new_hints.size());
            co_return;
        }

        uint64_t expected_reclaim = locs[0].second.total_size + locs[1].second.total_size;
        if (result.bytes_reclaimed != expected_reclaim)
        {
            failure_message = std::format("Expected reclaim {}, got {}", expected_reclaim, result.bytes_reclaimed);
            co_return;
        }

        if (fs::file_size(dst_path) != 0)
        {
            failure_message = std::format("Expected empty file, got size {}", fs::file_size(dst_path));
            co_return;
        }

        passed = true;
    };

    ctx.RunUntilDone(task());
    EXPECT_TRUE(passed) << failure_message;
}

TEST_F(CompactorTest, LargeEntryExceedsBuffers)
{
    // Scenario: Write a value larger than default I/O buffers (assuming ~16KB-64KB defaults).
    // This forces the compactor to resize buffers or handle partial chunks correctly.
    bool passed = false;
    std::string failure_message;

    auto task = [&]() -> kio::Task<>
    {
        // 1MB value
        std::string large_val = RandomString(1024 * 1024);
        auto locs = co_await CreateDataFile(100, {{"large_key", large_val}});

        auto& key_dir = io->GetKeyDir();
        key_dir["large_key"] = locs[0].second;

        const auto dst_path = io->GetDataFilePath(200);
        kio::Result<CompactionResult> result_opt;
        {
            const int dst_fd = open(dst_path.c_str(), config.write_flags, config.file_mode);
            if (dst_fd < 0)
            {
                failure_message = "Failed to open destination file";
                co_return;
            }
            auto shared_fd = std::make_shared<kio::FD>(dst_fd);
            DataFile dst_file(shared_fd, 200, config);

            result_opt = co_await compactor->CompactFiles(ctx, {100}, dst_file);
        }
        if (!result_opt.has_value())
        {
            failure_message = "Compact failed: " + result_opt.error().message();
            co_return;
        }
        auto result = result_opt.value();

        if (result.new_hints.size() != 1)
        {
            failure_message = "Expected 1 hint";
            co_return;
        }
        if (result.bytes_reclaimed != 0)
        {
            failure_message = "Expected 0 reclaimed";
            co_return;
        }

        auto entries = co_await ReadAllEntries(200);
        if (entries.size() != 1)
        {
            failure_message = "Expected 1 entry in output";
            co_return;
        }

        std::string val_out(reinterpret_cast<const char*>(entries[0].GetValueView().data()),
                            entries[0].GetValueView().size());

        if (val_out.size() != large_val.size())
        {
            failure_message = "Value size mismatch";
            co_return;
        }
        if (val_out != large_val)
        {
            failure_message = "Value content mismatch";
            co_return;
        }

        passed = true;
    };

    ctx.RunUntilDone(task());
    EXPECT_TRUE(passed) << failure_message;
}

TEST_F(CompactorTest, ManySmallEntriesBufferFlushing)
{
    // Scenario: Write many small entries to force the output buffer to flush multiple times.
    bool passed = false;
    std::string failure_message;

    auto task = [&]() -> kio::Task<>
    {
        std::vector<std::pair<std::string, std::string>> data;
        // 1000 entries * ~100 bytes = ~100KB.
        // If buffer is 16KB, this flushes ~6 times.
        for (int i = 0; i < 1000; ++i)
        {
            data.push_back({std::format("k_{}", i), std::format("val_{}", i)});
        }

        auto locs = co_await CreateDataFile(100, data);

        auto& key_dir = io->GetKeyDir();
        for (const auto& [k, loc] : locs) key_dir[k] = loc;

        auto dst_path = io->GetDataFilePath(200);
        kio::Result<CompactionResult> result_opt;
        {
            int dst_fd = open(dst_path.c_str(), config.write_flags, config.file_mode);
            if (dst_fd < 0)
            {
                failure_message = "Failed to open destination file";
                co_return;
            }
            auto shared_fd = std::make_shared<kio::FD>(dst_fd);
            DataFile dst_file(shared_fd, 200, config);

            result_opt = co_await compactor->CompactFiles(ctx, {100}, dst_file);
        }
        if (!result_opt.has_value())
        {
            failure_message = "Compact failed";
            co_return;
        }
        auto result = result_opt.value();

        if (result.new_hints.size() != 1000)
        {
            failure_message = std::format("Expected 1000 hints, got {}", result.new_hints.size());
            co_return;
        }

        auto entries = co_await ReadAllEntries(200);
        if (entries.size() != 1000)
        {
            failure_message = std::format("Expected 1000 entries, got {}", entries.size());
            co_return;
        }

        if (entries.back().GetKeyView() != "k_999")
        {
            failure_message = "Last entry key mismatch";
            co_return;
        }

        passed = true;
    };

    ctx.RunUntilDone(task());
    EXPECT_TRUE(passed) << failure_message;
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
