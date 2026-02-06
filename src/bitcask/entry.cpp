//
// Created by Yao ACHI on 03/02/2026.
//
#include "bitcask/entry.hpp"

#include "crc32c/crc32c.h"

namespace bitcask
{

// A private constructor for when we already have the raw blob
DataEntry::DataEntry(std::vector<std::byte>&& raw_blob)
    : payload_(std::move(raw_blob))
{
    auto* ptr = payload_.data();
    const auto key_len = ReadLe<uint32_t>(ptr + 13);
    const auto val_len = ReadLe<uint32_t>(ptr + 17);

    key_view_ = std::string_view(
                reinterpret_cast<const char*>(ptr) + kEntryFixedHeaderSize,
                key_len
            );

    value_view_ = std::span<const std::byte>(
        ptr + kEntryFixedHeaderSize + key_len,
        val_len
    );
}

DataEntry::DataEntry(std::string_view key,
                     std::span<const std::byte> value,
                     const uint8_t flag,
                     const uint64_t timestamp)
{
    const auto len = static_cast<uint32_t>(key.size());
    const auto v_len = static_cast<uint32_t>(value.size());

    payload_.resize(kEntryFixedHeaderSize + len + v_len);

    std::byte* base = payload_.data();

    // Write Headers using the OFFSET overload
    WriteLe(base, 0,  static_cast<uint32_t>(0));
    WriteLe(base, 4,  timestamp);
    WriteLe(base, 12, flag);
    WriteLe(base, 13, len);
    WriteLe(base, 17, v_len);

    auto* raw_ptr = reinterpret_cast<uint8_t*>(base);

    std::memcpy(raw_ptr + kEntryFixedHeaderSize, key.data(), len);
    std::memcpy(raw_ptr + kEntryFixedHeaderSize + len, value.data(), v_len);

    // CRC
    const uint32_t crc = crc32c::Crc32c(raw_ptr + 4, payload_.size() - 4);
    WriteLe(base, 0, crc);

    // Views
    key_view_ = std::string_view(
        reinterpret_cast<const char*>(base) + kEntryFixedHeaderSize,
        len
    );
    value_view_ = std::span<const std::byte>(base + kEntryFixedHeaderSize + len, v_len);
}

kio::Result<DataEntry> DataEntry::Deserialize(std::span<const std::byte> buffer)
{
    if (buffer.size() < kEntryFixedHeaderSize)
    {
        return std::unexpected(kio::ParseError::Incomplete);
    }

    const std::byte* base = buffer.data();

    // Decode Lengths to validate size
    const auto len = ReadLe<uint32_t>(base + 13);
    const auto v_len = ReadLe<uint32_t>(base + 17);
    const size_t total_size = kEntryFixedHeaderSize + len + v_len;

    if (buffer.size() < total_size)
    {
        return std::unexpected(kio::ParseError::Incomplete);
    }

    //    We check the CRC stored in the file against the computed CRC of the data.
    const auto stored_crc = ReadLe<uint32_t>(base);
    const uint8_t* crc_start = reinterpret_cast<const uint8_t*>(base) + 4;
    const auto computed_crc = crc32c::Crc32c(crc_start, total_size - 4);
    if (computed_crc != stored_crc)
    {
        return std::unexpected(kio::ParseError::Corrupted);
    }

    std::vector blob(buffer.begin(), buffer.begin() + kEntryFixedHeaderSize + len + v_len);
    return DataEntry(std::move(blob));
}

size_t HintEntry::SerializeTo(std::span<std::byte> out_buffer) const
{
    const auto key_len = static_cast<uint32_t>(key.size());
    const auto required_size = kHintHeaderSize + key_len;

    if (out_buffer.size() < required_size) {
        return 0;
    }

    auto* ptr = reinterpret_cast<uint8_t*>(out_buffer.data());

    WriteLe(ptr, timestamp_ns);
    WriteLe(ptr + 8, offset);
    WriteLe(ptr + 16, size);
    WriteLe(ptr + 20, key_len);

    if (key_len > 0) {
        std::memcpy(ptr + kHintHeaderSize, key.data(), key_len);
    }

    return required_size;
}

kio::Result<std::pair<HintEntry, size_t>> HintEntry::Deserialize(const std::span<const std::byte> buffer)
{
    if (buffer.size() < kHintHeaderSize)
    {
        return std::unexpected(kio::ParseError::Incomplete);
    }

    const std::byte* ptr = buffer.data();

    HintEntry entry;

    entry.timestamp_ns = ReadLe<uint64_t>(ptr);

    entry.offset = ReadLe<uint64_t>(ptr + 8);
    entry.size = ReadLe<uint32_t>(ptr + 16);

    const auto len = ReadLe<uint32_t>(ptr + 20);

    if (buffer.size() < kHintHeaderSize + len)
    {
        return std::unexpected(kio::ParseError::Incomplete);
    }

    if (len > 0)
    {
        entry.key.assign(reinterpret_cast<const char*>(ptr + kHintHeaderSize), len);
    }

    return std::make_pair(entry, kHintHeaderSize + len);
}
}  // namespace bitcask
