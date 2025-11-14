//
// Created by Yao ACHI on 08/11/2025.
//

#ifndef KIO_ENTRY_H
#define KIO_ENTRY_H
#include <chrono>
#include <expected>
#include <span>
#include <string>
#include <vector>
#include <ylt/struct_pack.hpp>

#include "common.h"
#include "core/include/errors.h"


namespace bitcask
{
    // On-disk entry format:
    // +----------+--------------+-------------------------------+
    // | CRC (4B) | SIZE (8B) | PAYLOAD (struct_pack serialized) |
    // +----------+--------------+-------------------------------+
    // CRC: CRC32C of PAYLOAD only
    // SIZE: Length of PAYLOAD in bytes (does NOT include CRC or SIZE)
    // Total entry size = 4 + 8 + SIZE

    // Hint Entry, fully serialize/deserialize by struct_pack:
    // +----------------+--------+-----------+----------------+
    // | timestamp_ns(8)| entry_pos | total_sz  | key(string) |
    // +----------------+--------+-----------+----------------+
    // - timestamp_ns : uint64_t nanosecond timestamp
    // - entry_pos: position of the serialized data entry in the datafile
    // - total_sz: Total size of the serialized data entry in the datafile
    // - key:

    // PAYLOAD (struct_pack serialization of Entry):
    // +----------------+--------+-----------+-------------+
    // | timestamp_ns(8)| flag(1)| key(var)  | value(var)  |
    // +----------------+--------+-----------+-------------+
    // - timestamp_ns : uint64_t nanosecond timestamp
    // - flag: uint8_t (e.g. tombstone bit)
    // - key: std::string (length-prefixed by struct_pack)
    // - value: std::vector<char> (length-prefixed by struct_pack)

    // Bitcask storage layout:
    // +-----------+       +-----------+       +-----------+
    // | Data File | ----> | Hint File | ----> |  KeyDir   |
    // +-----------+       +-----------+       +-----------+
    //   [CRC|SIZE|PAYLOAD]    key→(file_id, offset, sz, ts)   in-memory map
    // - Data files: append-only log of serialized entries
    // - Hint files: lightweight summaries for quick startup
    // - KeyDir: fast in-memory lookup for the latest key location

    struct DataEntry
    {
        std::uint64_t timestamp_ns{};
        uint8_t flag = FLAG_NONE;
        std::string key;
        std::vector<char> value;

        // struct_pack need this
        DataEntry() = default;

        DataEntry(std::string&& key, std::vector<char>&& value, uint8_t flag = FLAG_NONE);

        // Tombstone marker (for deletions)
        [[nodiscard]]
        bool is_tombstone() const
        {
            return flag & FLAG_TOMBSTONE;
        }

        // Serialize to buffer
        [[nodiscard]]
        std::vector<char> serialize() const;

        // Deserialize from buffer
        static kio::Result<DataEntry> deserialize(std::span<const char> buffer);
    };

    struct HintEntry
    {
        uint64_t timestamp_ns{};
        uint64_t entry_pos{};
        uint32_t total_sz{};
        std::string key;

        HintEntry() = default;

        HintEntry(const uint64_t timestamp_ns, const uint64_t entry_pos, const uint64_t total_sz, std::string&& key) :
            timestamp_ns(timestamp_ns), entry_pos(entry_pos), total_sz(total_sz), key(std::move(key))
        {
        }

        // Serialize to buffer
        [[nodiscard]]
        std::vector<char> serialize() const;

        // Deserialize from buffer
        static kio::Result<HintEntry> deserialize(std::span<const char> buffer);
    };

    // for struct_pack
    YLT_REFL(DataEntry, timestamp_ns, flag, key, value);
    YLT_REFL(HintEntry, timestamp_ns, entry_pos, total_sz, key)
}  // namespace bitcask

#endif  // KIO_ENTRY_H
