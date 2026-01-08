
//
// Created by Yao ACHI on 08/11/2025.
//

#ifndef KIO_ENTRY_H
#define KIO_ENTRY_H
#include "absl/container/flat_hash_map.h"
#include "common.h"
#include "kio/core/errors.h"

#include <chrono>
#include <expected>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace bitcask
{
// On-disk entry format:
// ---------------------------------------------------------
// DISK LAYOUT (21 Bytes Header + Data)
// [0-3] CRC32 (checksum of everything that follows)
// [4-11] Timestamp
// [12]   Flag
// [13-16] Key Length
// [17-20] Value Length
// [21...] Key Bytes
// [...  ] Value Bytes

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
//   [CRC|SIZE|PAYLOAD] key→(file_id, offset, sz, ts)   in-memory map
// - Data files: append-only log of serialized entries
// - Hint files: lightweight summaries for quick startup
// - KeyDir: fast in-memory lookup for the latest key location

class DataEntry
{
    // This holds: [crc(4)][Timestamp(8)][Flag(1)][KeyLen(4)][KeyBytes...][ValueLen(4)][ValueBytes...]
    std::vector<char> payload_;

    std::string_view key_view_;
    std::span<const char> value_view_;

public:
    /**
     * @brief
     * @warning Protect yourself from DDOS; the caller must ensure the buffer is a reasonable size
     * @param buffer
     * @return
     */
    static kio::Result<DataEntry> Deserialize(std::span<const char> buffer);
    DataEntry(std::string_view key, std::span<const char> value, uint8_t flag, uint64_t timestamp);

    [[nodiscard]] uint32_t GetCrc() const { return ReadLe<uint32_t>(payload_.data()); }

    [[nodiscard]] uint64_t GetTimestamp() const { return ReadLe<uint64_t>(payload_.data() + 4); }

    [[nodiscard]] uint8_t GetFlag() const { return ReadLe<uint8_t>(payload_.data() + 12); }

    [[nodiscard]] std::string_view GetKeyView() const { return key_view_; }
    [[nodiscard]] std::span<const char> GetValueView() const { return value_view_; }
    [[nodiscard]] std::vector<char> GetValueOwned() const { return {value_view_.begin(), value_view_.end()}; }

    // Tombstone marker (for deletions)
    [[nodiscard]] bool IsTombstone() const { return (GetFlag() & kFlagTombstone) != 0; }

    void SetKeyView(std::string_view key) { key_view_ = key; }
    void SetValueView(std::span<const char> value) { value_view_ = value; }

    [[nodiscard]] std::span<const char> GetPayload() const { return payload_; }
    [[nodiscard]] uint32_t Size() const { return payload_.size(); }
};

struct HintEntry
{
    // Disk Layout (Little Endian):
    // [0-7]   Timestamp (ns)
    // [8-11]  Offset
    // [12-15] Size
    // [16-19] Key Length
    // [20...] Key Bytes

    uint32_t offset{};
    uint32_t size{};
    uint64_t timestamp_ns{};
    std::string key;

    [[nodiscard]] size_t Size() const { return kHintHeaderSize + key.size(); }

    HintEntry() = default;

    HintEntry(const uint64_t timestamp_ns, const uint64_t offset, const uint32_t size, std::string&& key)
        : offset(offset), size(size), timestamp_ns(timestamp_ns), key(std::move(key))
    {
    }

    // Serialize to buffer
    [[nodiscard]]
    std::vector<char> Serialize() const;

    /// Deserialize from buffer, returns the entry and its serialized size
    /// Deserializing give access to that size returning it makes sense to not have to recompute it
    static kio::Result<std::pair<HintEntry, size_t>> Deserialize(std::span<const char> buffer);
};

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

/// Value location in data file
struct ValueLocation
{
    uint64_t file_id{};
    uint64_t offset{};
    uint32_t total_size{};
    uint64_t timestamp_ns = GetCurrentTimestamp();
};

// Use Abseil's flat_hash_map with full strings for keys.
// This handles collisions correctly and is cache-friendly.
// Abseil supports heterogeneous lookup (string_view for std::string keys) automatically.
using KeyDir = absl::flat_hash_map<std::string, ValueLocation>;

}  // namespace bitcask

#endif  // KIO_ENTRY_H
