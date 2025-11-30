//
// Created by Yao ACHI on 08/11/2025.
//

#include "bitcask/include/entry.h"

#include <ylt/struct_pack.hpp>

#include "crc32c/crc32c.h"
#include "kio/include/async_logger.h"

namespace bitcask
{
    using namespace kio;

    DataEntry::DataEntry(std::string&& key, std::vector<char>&& value, const uint8_t flag) :
        timestamp_ns(get_current_timestamp<std::chrono::nanoseconds>()), flag(flag), key(std::move(key)), value(std::move(value))
    {
    }

    std::vector<char> DataEntry::serialize() const
    {
        // 1. Serialize the payload first to a temporary buffer.
        const auto payload_buffer = struct_pack::serialize(*this);
        const auto payload_size = payload_buffer.size();
        const auto total_size = kEntryFixedHeaderSize + payload_size;

        // 2. Calculate CRC.
        const uint32_t crc = crc32c::Crc32c(payload_buffer.data(), payload_buffer.size());

        // 3. Prepare header values for little-endian storage.
        uint32_t crc_le = crc;
        uint64_t size_le = payload_size;
        if constexpr (std::endian::native == std::endian::big)
        {
            crc_le = std::byteswap(crc_le);
            size_le = std::byteswap(size_le);
        }

        // 4. Construct the final buffer: [CRC|SIZE|PAYLOAD] directly.
        std::vector<char> buffer;
        buffer.reserve(total_size);

        // Append CRC (4 bytes)
        const char* crc_ptr = reinterpret_cast<const char*>(&crc_le);
        buffer.insert(buffer.end(), crc_ptr, crc_ptr + sizeof(crc_le));

        // Append SIZE (8 bytes)
        const char* size_ptr = reinterpret_cast<const char*>(&size_le);
        buffer.insert(buffer.end(), size_ptr, size_ptr + sizeof(size_le));

        // Append payload
        buffer.insert(buffer.end(), payload_buffer.begin(), payload_buffer.end());

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

        DataEntry recovered = std::move(entry.value());
        const auto entry_packed_size = struct_pack::get_needed_size(recovered);
        return std::make_pair(std::move(recovered), kEntryFixedHeaderSize + entry_packed_size);
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
