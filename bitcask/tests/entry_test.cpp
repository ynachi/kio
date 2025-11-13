//
// Created by Yao ACHI on 09/11/2025.
//

#include "bitcask/include/entry.h"

#include <cerrno>
#include <gtest/gtest.h>
#include <span>
#include <thread>
#include <vector>
#include <ylt/struct_pack.hpp>

#include "bitcask/include/common.h"
#include "crc32c/crc32c.h"

using namespace bitcask;

class EntryTest : public ::testing::Test
{
protected:
    // Helper to create a standard test entry
    static Entry create_standard_entry() { return Entry("test_key", {'v', 'a', 'l', 'u', 'e'}); }
};

// --- Basic Construction & Flags ---

TEST_F(EntryTest, ConstructorSetsTimestamp)
{
    auto before = get_current_timestamp_ns();
    // Small sleep to ensure a clock moves if resolution is low
    std::this_thread::sleep_for(std::chrono::nanoseconds(1));
    Entry entry("key", {});
    std::this_thread::sleep_for(std::chrono::nanoseconds(1));
    auto after = get_current_timestamp_ns();

    EXPECT_GE(entry.timestamp_ns, before);
    EXPECT_LE(entry.timestamp_ns, after);
    EXPECT_EQ(entry.key, "key");
    EXPECT_TRUE(entry.value.empty());
    EXPECT_EQ(entry.flag, FLAG_NONE);
    EXPECT_FALSE(entry.is_tombstone());
}

TEST_F(EntryTest, TombstoneFlagWorks)
{
    const Entry entry("deleted_key", {}, FLAG_TOMBSTONE);
    EXPECT_TRUE(entry.is_tombstone());
    EXPECT_EQ(entry.flag, FLAG_TOMBSTONE);
}

TEST_F(EntryTest, IsTombstoneExplicitCheck)
{
    const Entry normal_entry("key1", {'a'});
    EXPECT_FALSE(normal_entry.is_tombstone());

    const Entry tombstone_entry("key2", {}, FLAG_TOMBSTONE);
    EXPECT_TRUE(tombstone_entry.is_tombstone());
}

// --- Serialization & Format verification ---

TEST_F(EntryTest, SerializationFormatHeaderCheck)
{
    // This test verifying the on-disk format is exactly what we expect:
    // [CRC (4B LE)][SIZE (8B LE)][PAYLOAD...]
    const Entry entry("k", {'v'});
    auto data = entry.serialize();

    ASSERT_GE(data.size(), MIN_ON_DISK_SIZE);

    const auto stored_crc = read_le<u_int32_t>(data.data());
    const auto stored_size = read_le<uint64_t>(data.data() + 4);

    // payload size matches what struct_pack gives
    const auto payload_size = struct_pack::get_needed_size(entry);
    EXPECT_GE(payload_size, stored_size);

    // Verify CRC matches manual calculation of payload
    // IMPORTANT: We must only CRC the 'stored_size' bytes, ignoring any extra capacity in 'data'.
    const std::span<const char> payload_span(data.data() + MIN_ON_DISK_SIZE, stored_size);
    const auto expected_crc = crc32c::Crc32c(payload_span.data(), payload_span.size());
    EXPECT_EQ(stored_crc, expected_crc);
}

// --- Round-Trip Scenarios ---

TEST_F(EntryTest, SerializeDeserializeRoundTrip)
{
    const Entry original("my_key", {'v', 'a', 'l', '1', '2', '3'});
    const auto payload_size = struct_pack::get_needed_size(original);
    std::cout << "payload size from struct pack " << payload_size << std::endl;

    auto buffer = original.serialize();
    auto result = Entry::deserialize(buffer);
    ASSERT_TRUE(result.has_value());

    const auto& deserialized = result.value();

    EXPECT_EQ(original.timestamp_ns, deserialized.timestamp_ns);
    EXPECT_EQ(original.flag, deserialized.flag);
    EXPECT_EQ(original.key, deserialized.key);
    EXPECT_EQ(original.value, deserialized.value);
}

TEST_F(EntryTest, TombstoneRoundTrip)
{
    const Entry tombstone("deleted_key", {}, FLAG_TOMBSTONE);

    auto buffer = tombstone.serialize();
    const auto result = Entry::deserialize(buffer);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().is_tombstone());
    EXPECT_EQ(result.value().key, "deleted_key");
}

TEST_F(EntryTest, TimestampPreservation)
{
    constexpr uint64_t expected_ts = 987654321987654321ULL;
    Entry entry("time_test", {'t'});
    entry.timestamp_ns = expected_ts;

    auto buffer = entry.serialize();
    const auto result = Entry::deserialize(buffer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().timestamp_ns, expected_ts);
}

// --- Edge Case Data ---

TEST_F(EntryTest, EmptyKeyAndValue)
{
    const Entry entry("", {});

    auto buffer = entry.serialize();
    const auto result = Entry::deserialize(buffer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().key, "");
    EXPECT_TRUE(result.value().value.empty());
}

TEST_F(EntryTest, LargeValue)
{
    std::vector large_value(10000, 'x');
    const Entry entry("large_key", std::move(large_value));

    auto buffer = entry.serialize();
    const auto result = Entry::deserialize(buffer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().key, "large_key");
    EXPECT_EQ(result.value().value.size(), 10000);
}

TEST_F(EntryTest, SpecialCharactersAndBinaryData)
{
    using namespace std::string_literals;

    std::string key = "key\0with\nnull\tand\rspecial"s;

    ASSERT_EQ(key.size(), 25) << "Key was truncated! Did you forget the 's' suffix?";

    // Save a copy before moving
    std::string expected_key = key;

    std::vector<char> value;
    for (int i = 0; i < 256; ++i)
    {
        value.push_back(static_cast<char>(i));
    }

    // Now move into Entry
    Entry entry(std::move(key), std::move(value));
    auto buffer = entry.serialize();
    auto result = Entry::deserialize(buffer);

    ASSERT_TRUE(result.has_value());
    const auto& deserialized = result.value();

    // Compare against the copy we saved
    EXPECT_EQ(deserialized.key, expected_key);
    EXPECT_EQ(deserialized.key.size(), 25);
    EXPECT_EQ(deserialized.value.size(), 256);
}

// --- Buffer Handling Scenarios ---

TEST_F(EntryTest, MultipleEntriesInBuffer)
{
    Entry entry1("key1", {'a'});
    Entry entry2("key2", {'b', 'c'});

    auto buffer1 = entry1.serialize();
    auto buffer2 = entry2.serialize();

    std::vector<char> combined = buffer1;
    combined.insert(combined.end(), buffer2.begin(), buffer2.end());

    // Read first
    auto result1 = Entry::deserialize(combined);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1->key, "key1");

    // Read second using offset
    std::span<const char> remaining(combined.data() + buffer1.size(), combined.size() - buffer1.size());
    auto result2 = Entry::deserialize(remaining);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->key, "key2");
}

TEST_F(EntryTest, DeserializeBufferWithExtraBytes)
{
    const Entry entry("test", {'x'});
    auto buffer = entry.serialize();

    const uint64_t true_size = MIN_ON_DISK_SIZE + read_le<uint64_t>(buffer.data() + 4);

    // Add garbage at the end
    buffer.insert(buffer.end(), 50, 0xFF);

    const auto result = Entry::deserialize(buffer);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->key, "test");
}

// --- Error Handling & Security ---

TEST_F(EntryTest, DeserializeFailsOnTooSmallBuffer)
{
    std::vector<char> tiny_buffer(MIN_ON_DISK_SIZE - 1, 0x00);
    auto result = Entry::deserialize(tiny_buffer);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().errno_value, EINVAL);
}

TEST_F(EntryTest, DeserializeFailsOnDataCorruption)
{
    Entry entry = create_standard_entry();
    auto data = entry.serialize();

    ASSERT_GT(data.size(), MIN_ON_DISK_SIZE);
    data[MIN_ON_DISK_SIZE] ^= 0xFF;

    auto result = Entry::deserialize(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), kio::Error::from_category(kio::IoError::IODataCorrupted));
}

TEST_F(EntryTest, DeserializeFailsOnHeaderCorruption)
{
    Entry entry = create_standard_entry();
    auto data = entry.serialize();

    data[0] ^= 0xFF;

    auto result = Entry::deserialize(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), kio::Error::from_category(kio::IoError::IODataCorrupted));
}

TEST_F(EntryTest, DeserializeSafelyHandlesIncompletePayload)
{
    Entry entry = create_standard_entry();
    auto valid_data = entry.serialize();

    // Header claims full size, but we only provide the header.
    std::vector<char> incomplete_data(valid_data.begin(), valid_data.begin() + MIN_ON_DISK_SIZE);

    auto result = Entry::deserialize(incomplete_data);
    ASSERT_FALSE(result.has_value());
    // Should fail because it knows it needs more data than is available
    EXPECT_EQ(result.error().errno_value, EINVAL);
}
