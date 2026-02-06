//
// Created by Yao ACHI on 05/02/2026.
//

#include <gtest/gtest.h>
#include <bitcask/entry.hpp>

using namespace bitcask;


// Helper to inspect raw bytes of a DataEntry
std::vector<std::byte> ToVector(const DataEntry& entry) {
    auto span = entry.GetPayloadSpan();
    return {span.begin(), span.end()};
}

TEST(EntryTest, RoundTrip_ValidData) {
    std::string key = "user:123";
    std::string val = R"({"name": "alice"})";
    const auto ts = GetCurrentTimestamp();

    // internally serialized
    const DataEntry original(key, std::as_bytes(std::span(val)), kFlagNone, ts);

    // Deserialize
    const auto result = DataEntry::Deserialize(original.GetPayloadSpan());
    ASSERT_TRUE(result.has_value());

    const DataEntry& decoded = result.value();

    // Verify
    EXPECT_EQ(decoded.GetKeyView(), key);
    // Convert bytes back to string for comparison
    const std::string decoded_val(reinterpret_cast<const char*>(decoded.GetValueView().data()), decoded.GetValueView().size());
    EXPECT_EQ(decoded_val, val);
    EXPECT_EQ(decoded.GetTimestamp(), ts);
    EXPECT_EQ(decoded.GetFlag(), kFlagNone);
    EXPECT_FALSE(decoded.IsTombstone());
}

TEST(EntryTest, Tombstone_Flag) {
    const DataEntry entry("del_key", {}, kFlagTombstone);
    EXPECT_TRUE(entry.IsTombstone());
    EXPECT_EQ(entry.GetFlag(), kFlagTombstone);
}

TEST(EntryTest, Deserialize_CorruptedCrc) {
    std::string val = "val";
    DataEntry original("key", std::as_bytes(std::span(val)));
    auto raw = ToVector(original);

    // Corrupt the last byte of the payload (part of value)
    raw.back() ^= std::byte{0xFF};

    auto result = DataEntry::Deserialize(raw);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), kio::ParseError::Corrupted);
}

TEST(EntryTest, Deserialize_CorruptedHeader) {
    std::string val = "val";
    DataEntry original("key", std::as_bytes(std::span(val)));
    auto raw = ToVector(original);

    // Corrupt the timestamp (offset 4)
    // Note: This changes the data used to compute CRC, but the Stored CRC (offset 0) remains valid for the OLD data.
    // Thus, the computed CRC of this new data will mismatch the stored CRC.
    raw[4] = std::byte{0xFF};

    auto result = DataEntry::Deserialize(raw);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), kio::ParseError::Corrupted);
}

TEST(EntryTest, Deserialize_IncompleteBuffer) {
    std::string val = "long_value";
    DataEntry original("long_key", std::as_bytes(std::span(val)));
    auto raw = ToVector(original);

    // Truncate payload
    std::vector truncated(raw.begin(), raw.end() - 1);
    EXPECT_EQ(DataEntry::Deserialize(truncated).error(), kio::ParseError::Incomplete);

    // Buffer smaller than fixed header
    std::vector header_only(raw.begin(), raw.begin() + 10);
    EXPECT_EQ(DataEntry::Deserialize(header_only).error(), kio::ParseError::Incomplete);
}

TEST(EntryTest, Deserialize_ZeroLength) {
    DataEntry empty_val("key", {});
    auto result = DataEntry::Deserialize(empty_val.GetPayloadSpan());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValueView().size(), 0);
}

TEST(EntryTest, HintEntry_RoundTrip) {
    HintEntry original;
    original.timestamp_ns = 123456789;
    original.offset = 4096;
    original.size = 128;
    original.key = "my_hint_key";

    // 1. Serialize
    std::vector<std::byte> buffer(original.Size());
    size_t written = original.SerializeTo(buffer);
    EXPECT_EQ(written, buffer.size());

    // 2. Deserialize
    auto res = HintEntry::Deserialize(buffer);
    ASSERT_TRUE(res.has_value());

    auto [decoded, size] = *res;
    EXPECT_EQ(size, written);
    EXPECT_EQ(decoded.timestamp_ns, original.timestamp_ns);
    EXPECT_EQ(decoded.offset, original.offset);
    EXPECT_EQ(decoded.size, original.size);
    EXPECT_EQ(decoded.key, original.key);
}

TEST(EntryTest, HintEntry_BufferTooSmall) {
    HintEntry entry(1, 100, 50, "key");
    std::vector<std::byte> buffer(entry.Size() - 1); // Too small

    // SerializeTo returns 0 on small buffer
    EXPECT_EQ(entry.SerializeTo(buffer), 0);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}