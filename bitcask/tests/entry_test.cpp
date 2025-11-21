// //
// // Created by Yao ACHI on 09/11/2025.
// //
//
// #include "bitcask/include/entry.h"
//
// #include <cerrno>
// #include <gtest/gtest.h>
// #include <span>
// #include <thread>
// #include <vector>
// #include <ylt/struct_pack.hpp>
//
// #include "bitcask/include/common.h"
// #include "crc32c/crc32c.h"
//
// using namespace bitcask;
//
// class EntryTest : public ::testing::Test
// {
// protected:
//     // Helper to create a standard test entry
//     static DataEntry create_standard_entry() { return DataEntry("test_key", {'v', 'a', 'l', 'u', 'e'}); }
// };
//
// // --- Basic Construction & Flags ---
//
// TEST_F(EntryTest, ConstructorSetsTimestamp)
// {
//     auto before = get_current_timestamp();
//     // Small sleep to ensure a clock moves if resolution is low
//     std::this_thread::sleep_for(std::chrono::nanoseconds(1));
//     DataEntry entry("key", {});
//     std::this_thread::sleep_for(std::chrono::nanoseconds(1));
//     auto after = get_current_timestamp();
//
//     EXPECT_GE(entry.timestamp_ns, before);
//     EXPECT_LE(entry.timestamp_ns, after);
//     EXPECT_EQ(entry.key, "key");
//     EXPECT_TRUE(entry.value.empty());
//     EXPECT_EQ(entry.flag, kFlagNone);
//     EXPECT_FALSE(entry.is_tombstone());
// }
//
// TEST_F(EntryTest, TombstoneFlagWorks)
// {
//     const DataEntry entry("deleted_key", {}, kFlagTombstone);
//     EXPECT_TRUE(entry.is_tombstone());
//     EXPECT_EQ(entry.flag, kFlagTombstone);
// }
//
// TEST_F(EntryTest, IsTombstoneExplicitCheck)
// {
//     const DataEntry normal_entry("key1", {'a'});
//     EXPECT_FALSE(normal_entry.is_tombstone());
//
//     const DataEntry tombstone_entry("key2", {}, kFlagTombstone);
//     EXPECT_TRUE(tombstone_entry.is_tombstone());
// }
//
// // --- Serialization & Format verification ---
//
// TEST_F(EntryTest, SerializationFormatHeaderCheck)
// {
//     // This test verifying the on-disk format is exactly what we expect:
//     // [CRC (4B LE)][SIZE (8B LE)][PAYLOAD...]
//     const DataEntry entry("k", {'v'});
//     auto data = entry.serialize();
//
//     ASSERT_GE(data.size(), kEntryFixedHeaderSize);
//
//     const auto stored_crc = read_le<u_int32_t>(data.data());
//     const auto stored_size = read_le<uint64_t>(data.data() + 4);
//
//     // payload size matches what struct_pack gives
//     const auto payload_size = struct_pack::get_needed_size(entry);
//     EXPECT_GE(payload_size, stored_size);
//
//     // Verify CRC matches manual calculation of payload
//     // IMPORTANT: We must only CRC the 'stored_size' bytes, ignoring any extra capacity in 'data'.
//     const std::span<const char> payload_span(data.data() + kEntryFixedHeaderSize, stored_size);
//     const auto expected_crc = crc32c::Crc32c(payload_span.data(), payload_span.size());
//     EXPECT_EQ(stored_crc, expected_crc);
// }
//
// // --- Round-Trip Scenarios ---
//
// TEST_F(EntryTest, SerializeDeserializeRoundTrip)
// {
//     const DataEntry original("my_key", {'v', 'a', 'l', '1', '2', '3'});
//     const auto payload_size = struct_pack::get_needed_size(original);
//     std::cout << "payload size from struct pack " << payload_size << std::endl;
//
//     auto buffer = original.serialize();
//     auto result = DataEntry::deserialize(buffer);
//     ASSERT_TRUE(result.has_value());
//
//     const auto& deserialized = result.value();
//
//     EXPECT_EQ(original.timestamp_ns, deserialized.timestamp_ns);
//     EXPECT_EQ(original.flag, deserialized.flag);
//     EXPECT_EQ(original.key, deserialized.key);
//     EXPECT_EQ(original.value, deserialized.value);
// }
//
// TEST_F(EntryTest, TombstoneRoundTrip)
// {
//     const DataEntry tombstone("deleted_key", {}, kFlagTombstone);
//
//     auto buffer = tombstone.serialize();
//     const auto result = DataEntry::deserialize(buffer);
//
//     ASSERT_TRUE(result.has_value());
//     EXPECT_TRUE(result.value().is_tombstone());
//     EXPECT_EQ(result.value().key, "deleted_key");
// }
//
// TEST_F(EntryTest, TimestampPreservation)
// {
//     constexpr uint64_t expected_ts = 987654321987654321ULL;
//     DataEntry entry("time_test", {'t'});
//     entry.timestamp_ns = expected_ts;
//
//     auto buffer = entry.serialize();
//     const auto result = DataEntry::deserialize(buffer);
//
//     ASSERT_TRUE(result.has_value());
//     EXPECT_EQ(result.value().timestamp_ns, expected_ts);
// }
//
// // --- Edge Case Data ---
//
// TEST_F(EntryTest, EmptyKeyAndValue)
// {
//     const DataEntry entry("", {});
//
//     auto buffer = entry.serialize();
//     const auto result = DataEntry::deserialize(buffer);
//
//     ASSERT_TRUE(result.has_value());
//     EXPECT_EQ(result.value().key, "");
//     EXPECT_TRUE(result.value().value.empty());
// }
//
// TEST_F(EntryTest, LargeValue)
// {
//     std::vector large_value(10000, 'x');
//     const DataEntry entry("large_key", std::move(large_value));
//
//     auto buffer = entry.serialize();
//     const auto result = DataEntry::deserialize(buffer);
//
//     ASSERT_TRUE(result.has_value());
//     EXPECT_EQ(result.value().key, "large_key");
//     EXPECT_EQ(result.value().value.size(), 10000);
// }
//
// TEST_F(EntryTest, SpecialCharactersAndBinaryData)
// {
//     using namespace std::string_literals;
//
//     std::string key = "key\0with\nnull\tand\rspecial"s;
//
//     ASSERT_EQ(key.size(), 25) << "Key was truncated! Did you forget the 's' suffix?";
//
//     // Save a copy before moving
//     std::string expected_key = key;
//
//     std::vector<char> value;
//     for (int i = 0; i < 256; ++i)
//     {
//         value.push_back(static_cast<char>(i));
//     }
//
//     // Now move into Entry
//     DataEntry entry(std::move(key), std::move(value));
//     auto buffer = entry.serialize();
//     auto result = DataEntry::deserialize(buffer);
//
//     ASSERT_TRUE(result.has_value());
//     const auto& deserialized = result.value();
//
//     // Compare against the copy we saved
//     EXPECT_EQ(deserialized.key, expected_key);
//     EXPECT_EQ(deserialized.key.size(), 25);
//     EXPECT_EQ(deserialized.value.size(), 256);
// }
//
// // --- Buffer Handling Scenarios ---
//
// TEST_F(EntryTest, MultipleEntriesInBuffer)
// {
//     DataEntry entry1("key1", {'a'});
//     DataEntry entry2("key2", {'b', 'c'});
//
//     auto buffer1 = entry1.serialize();
//     auto buffer2 = entry2.serialize();
//
//     std::vector<char> combined = buffer1;
//     combined.insert(combined.end(), buffer2.begin(), buffer2.end());
//
//     // Read first
//     auto result1 = DataEntry::deserialize(combined);
//     ASSERT_TRUE(result1.has_value());
//     EXPECT_EQ(result1->key, "key1");
//
//     // Read second using offset
//     std::span<const char> remaining(combined.data() + buffer1.size(), combined.size() - buffer1.size());
//     auto result2 = DataEntry::deserialize(remaining);
//     ASSERT_TRUE(result2.has_value());
//     EXPECT_EQ(result2->key, "key2");
// }
//
// TEST_F(EntryTest, DeserializeBufferWithExtraBytes)
// {
//     const DataEntry entry("test", {'x'});
//     auto buffer = entry.serialize();
//
//     const uint64_t true_size = kEntryFixedHeaderSize + read_le<uint64_t>(buffer.data() + 4);
//
//     // Add garbage at the end
//     buffer.insert(buffer.end(), 50, 0xFF);
//
//     const auto result = DataEntry::deserialize(buffer);
//     ASSERT_TRUE(result.has_value());
//     EXPECT_EQ(result->key, "test");
// }
//
// // --- Error Handling & Security ---
//
// TEST_F(EntryTest, DeserializeFailsOnTooSmallBuffer)
// {
//     std::vector<char> tiny_buffer(kEntryFixedHeaderSize - 1, 0x00);
//     auto result = DataEntry::deserialize(tiny_buffer);
//
//     ASSERT_FALSE(result.has_value());
//     EXPECT_EQ(result.error().errno_value, EINVAL);
// }
//
// TEST_F(EntryTest, DeserializeFailsOnDataCorruption)
// {
//     DataEntry entry = create_standard_entry();
//     auto data = entry.serialize();
//
//     ASSERT_GT(data.size(), kEntryFixedHeaderSize);
//     data[kEntryFixedHeaderSize] ^= 0xFF;
//
//     auto result = DataEntry::deserialize(data);
//     ASSERT_FALSE(result.has_value());
//     EXPECT_EQ(result.error(), kio::Error::from_category(kio::IoError::IODataCorrupted));
// }
//
// TEST_F(EntryTest, DeserializeFailsOnHeaderCorruption)
// {
//     DataEntry entry = create_standard_entry();
//     auto data = entry.serialize();
//
//     data[0] ^= 0xFF;
//
//     auto result = DataEntry::deserialize(data);
//     ASSERT_FALSE(result.has_value());
//     EXPECT_EQ(result.error(), kio::Error::from_category(kio::IoError::IODataCorrupted));
// }
//
// TEST_F(EntryTest, DeserializeSafelyHandlesIncompletePayload)
// {
//     DataEntry entry = create_standard_entry();
//     auto valid_data = entry.serialize();
//
//     // Header claims full size, but we only provide the header.
//     std::vector<char> incomplete_data(valid_data.begin(), valid_data.begin() + kEntryFixedHeaderSize);
//
//     auto result = DataEntry::deserialize(incomplete_data);
//     ASSERT_FALSE(result.has_value());
//     // Should fail because it knows it needs more data than is available
//     EXPECT_EQ(result.error().errno_value, EINVAL);
// }
//
// TEST_F(EntryTest, HintEntrySerializeDeserializeRoundTrip)
// {
//     // Create a hint entry with typical values
//     constexpr uint64_t timestamp_ns = 1234567890000000000ULL;
//     constexpr uint64_t entry_pos = 1024;
//     constexpr uint64_t total_sz = 256;
//     const std::string key = "test_key";
//
//     HintEntry original(timestamp_ns, entry_pos, total_sz, std::string(key));  // Copy key to avoid moving from const
//
//     // Serialize
//     auto buffer = original.serialize();
//     ASSERT_FALSE(buffer.empty());
//
//     // Deserialize
//     auto result = HintEntry::deserialize(buffer);
//     ASSERT_TRUE(result.has_value()) << "Deserialization failed";
//
//     const auto& deserialized = result.value();
//
//     // Verify all fields match
//     EXPECT_EQ(deserialized.timestamp_ns, timestamp_ns);
//     EXPECT_EQ(deserialized.entry_pos, entry_pos);
//     EXPECT_EQ(deserialized.total_sz, total_sz);
//     EXPECT_EQ(deserialized.key, key);
// }
//
// TEST_F(EntryTest, HintEntryEmptyKey)
// {
//     constexpr uint64_t timestamp_ns = 987654321ULL;
//     constexpr uint64_t entry_pos = 512;
//     constexpr uint64_t total_sz = 128;
//
//     HintEntry original(timestamp_ns, entry_pos, total_sz, std::string(""));  // Empty key
//
//     auto buffer = original.serialize();
//     auto result = HintEntry::deserialize(buffer);
//
//     ASSERT_TRUE(result.has_value());
//     EXPECT_EQ(result.value().key, "");
//     EXPECT_EQ(result.value().timestamp_ns, timestamp_ns);
//     EXPECT_EQ(result.value().entry_pos, entry_pos);
//     EXPECT_EQ(result.value().total_sz, total_sz);
// }
//
// TEST_F(EntryTest, HintEntryDeserializeFailsOnInvalidData)
// {
//     // Create invalid buffer data
//     std::vector<char> invalid_buffer = {0x00, 0x01, 0x02, 0x03};  // Too small and malformed
//
//     auto result = HintEntry::deserialize(invalid_buffer);
//
//     ASSERT_FALSE(result.has_value());
//     EXPECT_EQ(result.error(), kio::Error::from_category(kio::IoError::IODeserialization));
// }
//
// TEST_F(EntryTest, HintEntryWithLongKey)
// {
//     const std::string long_key(1000, 'x');  // 1000 character key
//     HintEntry original(1, 2, 3, std::string(long_key));
//
//     auto buffer = original.serialize();
//     auto result = HintEntry::deserialize(buffer);
//
//     ASSERT_TRUE(result.has_value());
//     EXPECT_EQ(result.value().key, long_key);
//     EXPECT_EQ(result.value().key.size(), 1000);
// }
//
// TEST_F(EntryTest, DeserializeMultipleHintEntriesFromBuffer)
// {
//     // Create multiple hint entries with different data
//     HintEntry entry1(1000, 1024, 128, std::string("key1"));
//     HintEntry entry2(2000, 2048, 256, std::string("key2"));
//     HintEntry entry3(3000, 4096, 512, std::string("key3_with_longer_name"));
//
//     // Serialize each entry
//     auto buffer1 = entry1.serialize();
//     auto buffer2 = entry2.serialize();
//     auto buffer3 = entry3.serialize();
//
//     // Combine all serialized data into one continuous buffer
//     std::vector<char> combined_buffer;
//     combined_buffer.insert(combined_buffer.end(), buffer1.begin(), buffer1.end());
//     combined_buffer.insert(combined_buffer.end(), buffer2.begin(), buffer2.end());
//     combined_buffer.insert(combined_buffer.end(), buffer3.begin(), buffer3.end());
//
//     // Deserialize entries one by one from the combined buffer
//     size_t offset = 0;
//
//     // First entry
//     std::span<const char> span1(combined_buffer.data() + offset, buffer1.size());
//     auto result1 = HintEntry::deserialize(span1);
//     ASSERT_TRUE(result1.has_value()) << "Failed to deserialize first hint entry";
//     EXPECT_EQ(result1->timestamp_ns, 1000);
//     EXPECT_EQ(result1->entry_pos, 1024);
//     EXPECT_EQ(result1->total_sz, 128);
//     EXPECT_EQ(result1->key, "key1");
//     offset += buffer1.size();
//
//     // Second entry
//     std::span<const char> span2(combined_buffer.data() + offset, buffer2.size());
//     auto result2 = HintEntry::deserialize(span2);
//     ASSERT_TRUE(result2.has_value()) << "Failed to deserialize second hint entry";
//     EXPECT_EQ(result2->timestamp_ns, 2000);
//     EXPECT_EQ(result2->entry_pos, 2048);
//     EXPECT_EQ(result2->total_sz, 256);
//     EXPECT_EQ(result2->key, "key2");
//     offset += buffer2.size();
//
//     // Third entry
//     std::span<const char> span3(combined_buffer.data() + offset, buffer3.size());
//     auto result3 = HintEntry::deserialize(span3);
//     ASSERT_TRUE(result3.has_value()) << "Failed to deserialize third hint entry";
//     EXPECT_EQ(result3->timestamp_ns, 3000);
//     EXPECT_EQ(result3->entry_pos, 4096);
//     EXPECT_EQ(result3->total_sz, 512);
//     EXPECT_EQ(result3->key, "key3_with_longer_name");
//     offset += buffer3.size();
//
//     // Verify we consumed the entire buffer
//     EXPECT_EQ(offset, combined_buffer.size());
// }
