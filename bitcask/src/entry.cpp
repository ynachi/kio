//
// Created by Yao ACHI on 08/11/2025.
//

#include "bitcask/include/entry.h"

#include <ylt/struct_pack.hpp>

#include "core/include/async_logger.h"
#include "crc32c/crc32c.h"

namespace bitcask
{
    using namespace kio;

    DataEntry::DataEntry(std::string&& key, std::vector<char>&& value, const uint8_t flag) :
        timestamp_ns(get_current_timestamp<std::chrono::nanoseconds>()), flag(flag), key(std::move(key)), value(std::move(value))
    {
    }

    std::vector<char> DataEntry::serialize() const
    {
        // Calculate the exact sizes needed ahead of time to avoid reallocations.
        const auto payload_size = struct_pack::get_needed_size(*this);
        const auto total_size = kEntryFixedHeaderSize + payload_size;

        std::vector<char> buffer;
        // Reserve the total capacity to avoid reallocation
        buffer.reserve(total_size);

        // Claim the 12-byte space for the header
        buffer.resize(kEntryFixedHeaderSize);

        // Append serialized data after the 12 bytes, reserved for CRC and payload size
        struct_pack::serialize_to(buffer, *this);

        const auto payload_span = std::span(buffer.data() + kEntryFixedHeaderSize, payload_size);
        const uint32_t crc = crc32c::Crc32c(payload_span.data(), payload_span.size());

        // Now fill CRC and size.
        uint32_t crc_le = crc;
        uint64_t size_le = payload_size;
        if constexpr (std::endian::native == std::endian::big)
        {
            crc_le = std::byteswap(crc_le);
            size_le = std::byteswap(size_le);
        }

        std::memcpy(buffer.data(), &crc_le, sizeof(crc_le));
        std::memcpy(buffer.data() + 4, &size_le, sizeof(size_le));

        return buffer;
    }

    Result<std::pair<DataEntry, uint64_t>> DataEntry::deserialize(std::span<const char> buffer)
    {
        // MIN_ON_DISK_SIZE == CRC SZ + PAYLOAD SZ
        if (buffer.size() <= kEntryFixedHeaderSize)
        {
            return std::unexpected(Error{ErrorCategory::Serialization, kIoNeedMoreData});
        }

        const auto crc = read_le<uint32_t>(buffer.data());
        const auto size = read_le<uint64_t>(buffer.data() + 4);

        if (buffer.size() < kEntryFixedHeaderSize + size)
        {
            return std::unexpected(Error{ErrorCategory::Serialization, kIoNeedMoreData});
        }

        const auto payload_span = buffer.subspan(kEntryFixedHeaderSize, size);
        if (crc32c::Crc32c(payload_span.data(), payload_span.size()) != crc)
        {
            return std::unexpected(Error{ErrorCategory::Serialization, kIoDataCorrupted});
        }

        auto entry = struct_pack::deserialize<DataEntry>(payload_span);
        if (!entry.has_value())
        {
            return std::unexpected(Error{ErrorCategory::Serialization, kIoDeserialization});
        }

        const auto entry_packed_size = struct_pack::get_needed_size(entry.value());
        return std::make_pair(std::move(entry.value()), kEntryFixedHeaderSize + entry_packed_size);
    }

    std::vector<char> HintEntry::serialize() const { return struct_pack::serialize(*this); }

    Result<HintEntry> HintEntry::deserialize(const std::span<const char> buffer)
    {
        // let struct_pack manage the error
        auto entry = struct_pack::deserialize<HintEntry>(buffer);
        if (!entry.has_value())
        {
            ALOG_ERROR("Failed to deserialize entry: {}", entry.error().message());
            return std::unexpected(Error{ErrorCategory::Serialization, kIoDeserialization});
        }
        return entry.value();
    }


}  // namespace bitcask
