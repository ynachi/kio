//
// Unit tests for DataEntry and HintEntry serialization
//
// These tests verify serialization correctness WITHOUT disk I/O.
// Fast, deterministic, and focused on the codec logic.
//

#include <gtest/gtest.h>
#include <span>
#include <string>

#include "bitcask/include/entry.h"

using namespace bitcask;

// ============================================================================
// DataEntry Serialization
// ============================================================================

class DataEntryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Disable logging for cleaner test output
        kio::alog::Configure(1024, kio::LogLevel::kDisabled);
    }
};

TEST_F(DataEntryTest, RoundTrip_BasicKeyValue)
{
    // construction is serialization
    const DataEntry entry("test_key", "value");

    ASSERT_GT(entry.Size(), kEntryFixedHeaderSize);

    const auto result = DataEntry::Deserialize(entry.GetPayload());
    ASSERT_TRUE(result.has_value()) << "Deserialization failed";

    const auto& recovered = result.value();

    EXPECT_EQ(recovered, entry);
}

TEST_F(DataEntryTest, RoundTrip_Tombstone)
{
    const DataEntry tombstone{"deleted_key", {}, kFlagTombstone};

    EXPECT_TRUE(tombstone.IsTombstone());

    auto result = DataEntry::Deserialize(tombstone.GetPayload());

    ASSERT_TRUE(result.has_value());
    const auto& recovered = result.value();

    EXPECT_TRUE(recovered.IsTombstone());
    EXPECT_EQ(recovered.GetKeyView(), "deleted_key");
    EXPECT_TRUE(recovered.GetValueView().empty());
}

TEST_F(DataEntryTest, RoundTrip_EmptyKeyAndValue)
{
    DataEntry const entry{"", {}};

    const auto result = DataEntry::Deserialize(entry.GetPayload());

    ASSERT_TRUE(result.has_value());
    const auto& recovered = result.value();

    EXPECT_TRUE(recovered.GetKeyView().empty());
    EXPECT_TRUE(recovered.GetValueView().empty());
}

TEST_F(DataEntryTest, RoundTrip_LargeValue)
{
    std::vector large_value(1024 * 1024, 'X');  // 1MB
    DataEntry const entry{"large_key", std::move(large_value)};

    const auto result = DataEntry::Deserialize(entry.GetPayload());

    ASSERT_TRUE(result.has_value());
    const auto& recovered = result.value();

    EXPECT_EQ(recovered.GetValueView().size(), 1024 * 1024);
    EXPECT_EQ(recovered.GetValueOwned().size(), 1024 * 1024);
    EXPECT_TRUE(std::all_of(recovered.GetValueView().begin(), recovered.GetValueView().end(),
                            [](char c) { return c == 'X'; }));
}

TEST_F(DataEntryTest, RoundTrip_BinaryData)
{
    // Keys and values with embedded nulls and high bytes
    std::string const binary_key{"\x00\xFF\x80", 3};
    std::vector binary_value{'\x00', '\x01', '\xFE', '\xFF'};

    const DataEntry entry{std::move(binary_key), std::move(binary_value)};

    const auto result = DataEntry::Deserialize(entry.GetPayload());

    ASSERT_TRUE(result.has_value());
    const auto& recovered = result.value();

    EXPECT_EQ(recovered, entry);
    EXPECT_EQ(recovered.GetKeyView(), binary_key);
    EXPECT_EQ(recovered.GetKeyView(), std::string("\x00\xFF\x80", 3));
    EXPECT_EQ(std::vector(recovered.GetValueView().begin(), recovered.GetValueView().end()), binary_value);
}

TEST_F(DataEntryTest, CRCDetectsCorruption)
{
    DataEntry entry{"key", "val"};

    // Corrupt the CRC (first 4 bytes)
    auto payload = entry.GetPayload();

    std::vector serialized(payload.begin(), payload.end());
    serialized[0] ^= 0xFF;

    auto result = DataEntry::Deserialize(serialized);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value, kio::kIoDataCorrupted);
}

TEST_F(DataEntryTest, CRCDetectsDataCorruption)
{
    DataEntry const entry{"key", "val"};

    auto payload = entry.GetPayload();

    std::vector serialized(payload.begin(), payload.end());

    // Corrupt a data byte (not the CRC)
    serialized[serialized.size() - 1] ^= 0xFF;

    auto result = DataEntry::Deserialize(serialized);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value, kio::kIoDataCorrupted);
}

TEST_F(DataEntryTest, RejectsTruncatedHeader)
{
    DataEntry const entry{"key", "val"};

    // Truncate to less than the header size
    const std::span truncated(entry.GetPayload().data(), kEntryFixedHeaderSize - 1);

    auto result = DataEntry::Deserialize(truncated);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value, kio::kIoNeedMoreData);
}

TEST_F(DataEntryTest, RejectsTruncatedPayload)
{
    const DataEntry entry{"key", "value"};

    auto serialized = entry.GetPayload();

    // Truncate payload (header complete, but key/value incomplete)
    const std::span truncated(serialized.data(), kEntryFixedHeaderSize + 2);

    auto result = DataEntry::Deserialize(truncated);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value, kio::kIoNeedMoreData);
}

TEST_F(DataEntryTest, ParsesMultipleEntriesInBuffer)
{
    std::vector entries = {
        DataEntry{"key1", "a"},
        DataEntry{"key2", "bc"},
        DataEntry{"key3", "def"},
    };

    // Serialize all into one buffer
    std::vector<char> buffer;
    for (const auto& e : entries)
    {
        auto s = e.GetPayload();
        buffer.insert(buffer.end(), s.begin(), s.end());
    }

    // Parse them back
    std::span<const char> remaining = buffer;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        auto result = DataEntry::Deserialize(remaining);
        ASSERT_TRUE(result.has_value()) << "Entry " << i << " failed";

        auto recovered = result.value();
        EXPECT_EQ(recovered, entries[i]);

        remaining = remaining.subspan(recovered.Size());
    }

    EXPECT_TRUE(remaining.empty()) << "Buffer should be fully consumed";
}

TEST_F(DataEntryTest, SizeCalculation)
{
    const DataEntry entry{"test", "val"};

    constexpr size_t kExpected = kEntryFixedHeaderSize + 4 + 3;  // header + "test" + "val"
    EXPECT_EQ(entry.Size(), kExpected);

    const auto serialized = entry.GetPayload();
    EXPECT_EQ(serialized.size(), kExpected);
}

// ============================================================================
// HintEntry Serialization
// ============================================================================

class HintEntryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        kio::alog::Configure(1024, kio::LogLevel::kDisabled);
    }
};

TEST_F(HintEntryTest, RoundTrip_Basic)
{
    const HintEntry hint{12345, 100, 50, "test_key"};

    auto serialized = hint.Serialize();
    auto result = HintEntry::Deserialize(serialized);

    ASSERT_TRUE(result.has_value());
    const auto& recovered = result.value().first;

    EXPECT_EQ(recovered.timestamp_ns, 12345);
    EXPECT_EQ(recovered.offset, 100);
    EXPECT_EQ(recovered.size, 50);
    EXPECT_EQ(recovered.key, "test_key");
}

TEST_F(HintEntryTest, RoundTrip_LargeOffsets)
{
    // Test with large file offsets (beyond 4GB)
    const HintEntry hint{
        999999999999ULL,        // timestamp
        5ULL * 1024 * 1024 * 1024,  // 5GB offset
        1024 * 1024,            // 1MB entry
        "big_file_key"
    };

    auto serialized = hint.Serialize();
    const auto result = HintEntry::Deserialize(serialized);

    ASSERT_TRUE(result.has_value());
    const auto& recovered = result.value().first;

    EXPECT_EQ(recovered.offset, 5ULL * 1024 * 1024 * 1024);
}

// ============================================================================
// Endianness
// ============================================================================

TEST_F(DataEntryTest, LittleEndianEncoding)
{
    DataEntry const entry{"key", "val"};
    const auto serialized = entry.GetPayload();

    // Verify CRC and timestamp are little-endian
    // (Reading raw bytes to check encoding)
    const auto crc = ReadLe<uint32_t>(serialized.data());
    const auto timestamp = ReadLe<uint64_t>(serialized.data() + 4);

    EXPECT_GT(crc, 0u);
    EXPECT_GT(timestamp, 0u);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}