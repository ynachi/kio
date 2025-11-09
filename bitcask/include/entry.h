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

    // PAYLOAD (struct_pack serialization of Entry):
    // +----------------+--------+-----------+-------------+
    // | timestamp_ns(8)| flag(1)| key(var)  | value(var)  |
    // +----------------+--------+-----------+-------------+
    // - timestamp_ns : uint64_t nanosecond timestamp
    // - flag: uint8_t (e.g. tombstone bit)
    // - key: std::string (length-prefixed by struct_pack)
    // - value: std::vector<char> (length-prefixed by struct_pack)

    // KeyDir entry (in-memory index):
    // +-----------+-------------+-------------+--------------+
    // | file_id(4)| offset(8)   | total_sz(8) | timestamp(8)*|
    // +-----------+-------------+-------------+--------------+
    // *timestamp optional (used for conflict resolution / merge)
    //
    // Maps: key → KeyDirEntry
    // file_id: Which data file the entry resides in
    // offset: Where [CRC|SIZE|PAYLOAD] starts in that file
    // total_sz: 4 + 8 + payload_size (total entry bytes on disk)

    // Bitcask storage layout:
    // +-----------+       +-----------+       +-----------+
    // | Data File | ----> | Hint File | ----> |  KeyDir   |
    // +-----------+       +-----------+       +-----------+
    //   [CRC|SIZE|PAYLOAD]    key→(file_id, offset, sz, ts)   in-memory map
    // - Data files: append-only log of serialized entries
    // - Hint files: lightweight summaries for quick startup
    // - KeyDir: fast in-memory lookup for the latest key location

    struct Entry
    {
        std::uint64_t timestamp_ns{};
        uint8_t flag = FLAG_NONE;
        std::string key;
        std::vector<char> value;

        // struct_pack need this
        Entry() = default;

        Entry(std::string&& key, std::vector<char>&& value, uint8_t flag = FLAG_NONE);

        // Tombstone marker (for deletions)
        [[nodiscard]]
        bool is_tombstone() const
        {
            return flag & FLAG_TOMBSTONE;
        }

        // Serialize to buffer
        [[nodiscard]]
        std::vector<char> serialize() const;

        // Deserialize from buffer, returns the cursor position upon successful deserialization
        static std::expected<std::pair<Entry, size_t>, kio::Error> deserialize(std::span<const char> buffer);
    };
    // for struct_pack
    YLT_REFL(Entry, timestamp_ns, flag, key, value);
}  // namespace bitcask

#endif  // KIO_ENTRY_H
