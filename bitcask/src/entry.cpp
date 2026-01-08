//
// Created by Yao ACHI on 08/11/2025.
//

#include "bitcask/include/entry.h"

#include "crc32c/crc32c.h"

namespace bitcask
{
using namespace kio;

DataEntry::DataEntry(std::string_view key, std::span<const char> value, const uint8_t flag, const uint64_t timestamp)
{
    const auto kLen = static_cast<uint32_t>(key.size());
    const auto kVLen = static_cast<uint32_t>(value.size());

    payload_.resize(kEntryFixedHeaderSize + kLen + kVLen);
    char* base = payload_.data();

    // Write header (leave CRC blank for now)
    WriteLe(base, static_cast<uint32_t>(0));  // CRC Placeholder
    WriteLe(base + 4, timestamp);
    WriteLe(base + 12, flag);
    WriteLe(base + 13, kLen);
    WriteLe(base + 17, kVLen);

    // Write key/value
    std::memcpy(base + kEntryFixedHeaderSize, key.data(), kLen);
    std::memcpy(base + kEntryFixedHeaderSize + kLen, value.data(), kVLen);

    // Compute CRC over everything except the CRC field itself
    const uint32_t kCrc = crc32c::Crc32c(base + 4, payload_.size() - 4);
    std::memcpy(base, &kCrc, sizeof(kCrc));

    // Views into payload
    key_view_ = std::string_view(base + kEntryFixedHeaderSize, kLen);
    value_view_ = std::span<const char>(base + kEntryFixedHeaderSize + kLen, kVLen);
}

Result<DataEntry> DataEntry::Deserialize(std::span<const char> buffer)
{
    if (buffer.size() < kEntryFixedHeaderSize)
    {
        return std::unexpected(Error(ErrorCategory::kSerialization, kIoNeedMoreData));
    }
    const char* base = buffer.data();

    // Decode Lengths to validate size
    const auto len = ReadLe<uint32_t>(base + 13);
    const auto v_len = ReadLe<uint32_t>(base + 17);

    if (buffer.size() < kEntryFixedHeaderSize + len + v_len)
    {
        return std::unexpected(Error(ErrorCategory::kSerialization, kIoNeedMoreData));
    }

    // crc check
    const auto stored_crc = ReadLe<uint32_t>(base);

    if (const auto computed_crc = crc32c::Crc32c(base + 4, kEntryFixedHeaderSize - 4 + len + v_len);
        computed_crc != stored_crc)
    {
        return std::unexpected(Error{ErrorCategory::kSerialization, kIoDataCorrupted});
    }

    // Decode Metadata
    const auto timestamp = ReadLe<uint64_t>(base + 4);
    const auto flag = ReadLe<uint8_t>(base + 12);

    // Create Entry
    std::string_view key(base + kEntryFixedHeaderSize, len);
    std::span value(base + kEntryFixedHeaderSize + len, v_len);

    return DataEntry(key, value, flag, timestamp);
}

std::vector<char> HintEntry::Serialize() const
{
    const auto len = static_cast<uint32_t>(key.size());
    std::vector<char> buffer(kHintHeaderSize + len);
    char* ptr = buffer.data();

    WriteLe(ptr, timestamp_ns);
    WriteLe(ptr + 8, offset);
    WriteLe(ptr + 12, size);
    WriteLe(ptr + 16, len);

    if (len > 0)
    {
        std::memcpy(ptr + kHintHeaderSize, key.data(), len);
    }
    return buffer;
}

Result<std::pair<HintEntry, size_t>> HintEntry::Deserialize(const std::span<const char> buffer)
{
    if (buffer.size() < kHintHeaderSize)
    {
        return std::unexpected(Error(ErrorCategory::kSerialization, kIoNeedMoreData));
    }

    const char* ptr = buffer.data();

    HintEntry entry;
    entry.timestamp_ns = ReadLe<uint64_t>(ptr);
    entry.offset = ReadLe<uint32_t>(ptr + 8);
    entry.size = ReadLe<uint32_t>(ptr + 12);

    const auto kLen = ReadLe<uint32_t>(ptr + 16);
    if (buffer.size() < kHintHeaderSize + kLen)
    {
        return std::unexpected(Error(ErrorCategory::kSerialization, kIoNeedMoreData));
    }

    if (kLen > 0)
    {
        entry.key.assign(ptr + kHintHeaderSize, kLen);
    }

    return std::make_pair(entry, kHintHeaderSize + kLen);
}

}  // namespace bitcask
