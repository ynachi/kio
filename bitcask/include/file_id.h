//
// Created by Yao ACHI on 24/11/2025.
//

#ifndef KIO_FILE_ID_H
#define KIO_FILE_ID_H

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

#include "../../kio/core/async_logger.h"

namespace bitcask
{
    /**
     * @brief File ID format: 64-bit unique identifier
     * * Layout:
     * ┌─────────────┬──────────────┬──────────────┐
     * │ Partition   │  Timestamp   │  Sequence    │
     * │  (16 bits)  │  (32 bits)   │  (16 bits)   │
     * └─────────────┴──────────────┴──────────────┘
     * * - Partition: 0-65535 (supports 64K partitions)
     * - Timestamp: Unix seconds (valid until year 2106)
     * - Sequence: 0-65535 (65K files per second per partition)
     */
    struct FileID
    {
        uint16_t partition;
        uint32_t timestamp_sec;
        uint16_t sequence;

        /**
         * @brief Encode to 64-bit file ID
         */
        [[nodiscard]] uint64_t encode() const { return (static_cast<uint64_t>(partition) << 48) | (static_cast<uint64_t>(timestamp_sec) << 16) | static_cast<uint64_t>(sequence); }

        /**
         * @brief Decode 64-bit file ID
         */
        static FileID decode(uint64_t id)
        {
            return FileID{.partition = static_cast<uint16_t>(id >> 48), .timestamp_sec = static_cast<uint32_t>((id >> 16) & 0xFFFFFFFF), .sequence = static_cast<uint16_t>(id & 0xFFFF)};
        }

        /**
         * @brief Human-readable format for debugging
         */
        [[nodiscard]] std::string debug() const { return std::format("FileId(partition={}, timestamp={}, seq={})", partition, timestamp_sec, sequence); }
    };

    /**
     * @brief Single-threaded file ID generator.
     * * Optimized for the Share-Nothing architecture.
     * NOT thread-safe. Must be owned by a single Partition/Worker.
     */
    class FileIdGenerator
    {
    public:
        explicit FileIdGenerator(const uint16_t partition_id) : partition_id_(partition_id), last_timestamp_(0), sequence_(0) {}

        /**
         * @brief Generate next file ID
         * Monotonically increasing. Handles clock skew.
         */
        uint64_t next()
        {
            auto now = get_current_timestamp_sec();

            if (now > last_timestamp_)
            {
                // New second - reset sequence
                last_timestamp_ = now;
                sequence_ = 0;
            }
            else
            {
                // Same second OR clock went backwards (skew).
                // Treat clock skew as "same second" to enforce monotonicity.
                now = last_timestamp_;

                if (sequence_ == 0xFFFF)
                {
                    // This is a rare edge case: generating >65k files in 1 second.
                    ALOG_WARN("FileIdGenerator: sequence overflow for partition {}", partition_id_);
                    sequence_ = 0;
                }
                else
                {
                    sequence_++;
                }
            }

            return FileID{partition_id_, now, sequence_}.encode();
        }

        [[nodiscard]] uint32_t current_timestamp() const { return last_timestamp_; }

        [[nodiscard]] uint16_t partition_id() const { return partition_id_; }

    private:
        uint16_t partition_id_;
        uint32_t last_timestamp_;
        uint16_t sequence_;

        static uint32_t get_current_timestamp_sec() { return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
    };

    /**
     * @brief Compare file IDs by timestamp (for sorting during recovery)
     */
    inline bool file_id_compare_by_time(uint64_t a, uint64_t b)
    {
        // Since the layout is Partition(16) | Time(32) | Seq(16),
        // direct integer comparison sorts by Partition, THEN Time, THEN Seq.
        // If we want to sort purely by Time (ignoring Partition), we need decoding, but why would we want that ?

        const auto id_a = FileID::decode(a);
        const auto id_b = FileID::decode(b);

        if (id_a.timestamp_sec != id_b.timestamp_sec) return id_a.timestamp_sec < id_b.timestamp_sec;

        return id_a.sequence < id_b.sequence;
    }

}  // namespace bitcask

#endif  // KIO_FILE_ID_H
