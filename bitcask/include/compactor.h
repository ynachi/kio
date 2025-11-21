//
// Created by Yao ACHI on 14/11/2025.
//

#ifndef KIO_COMPACTION_H
#define KIO_COMPACTION_H
#include "config.h"
#include "core/include/io/worker.h"
#include "data_file.h"
#include "hint_file.h"
#include "keydir.h"

/**
 * on disk, entries are like this [CRC(4B) | PAYLOAD_SIZE(8B) | PAYLOAD(variable)], repeated
 * Payload == Serialized(DataEntry)
 */
namespace bitcask
{
    /**
     * @brief Configuration limits for a compaction process.
     */
    struct CompactionLimits
    {
        size_t max_decode_buffer = 2 * 1024 * 1024;  // 2MB
        size_t max_hint_batch = 512 * 1024;  // 512KB
        // Max entries before KeyDir update
        size_t max_keydir_batch = 10000;
    };

    /**
     * @brief Manages Bitcask merge compaction (N-to-1).
     *
     * This compactor supports merging multiple source files into a single
     * destination file. The caller (database) manages the destination file
     * lifecycle, allowing efficient N-to-1 merging.
     *
     * USAGE PATTERN:
     * ```cpp
     * // Database creates destination files ONCE
     * auto [data_fd, hint_fd] = open_new_compaction_files(new_id);
     *     * // Compact multiple source files into them
     * for (auto src_id : old_files) {
     *     co_await compactor.compact_one(src_id, data_fd, hint_fd, new_id);
     * }
     *
     * // Database closes and finalizes
     * close(data_fd);
     * close(hint_fd);
     * ```
     *
     * THREAD SAFETY:
     * - The compactor runs on its own dedicated worker
     * - KeyDir operations are thread-safe
     * - Caller must ensure file descriptors are not shared across threads
     */
    class Compactor
    {
    public:
        /**
         * @brief Statistics collected during compaction.
         *
         * These are useful for monitoring, logging, and debugging.
         * They help answer questions like:
         * - How much space did we reclaim?
         * - How long did compaction take?
         * - What's the ratio of live to dead data?
         */
        struct Stats
        {
            uint64_t bytes_read = 0;
            uint64_t bytes_written = 0;
            uint64_t entries_kept = 0;
            uint64_t entries_discarded = 0;
            size_t files_compacted = 0;
            std::chrono::milliseconds duration{0};

            [[nodiscard]] double compression_ratio() const { return bytes_read > 0 ? static_cast<double>(bytes_written) / bytes_read : 0.0; }
            [[nodiscard]] double space_reclaimed_pct() const { return bytes_read > 0 ? 100.0 * (1.0 - compression_ratio()) : 0.0; }
        };

        /**
         * @param io_worker worker to drive the io loop
         * @param config Database configuration
         * @param keydir The shared in-memory index
         * @param limits Optional compaction limits
         */
        Compactor(kio::io::Worker& io_worker, BitcaskConfig& config, KeyDir& keydir, const CompactionLimits& limits = {});
        ~Compactor();

        // not movable, not copyable
        Compactor(const Compactor&) = delete;
        Compactor& operator=(const Compactor&) = delete;
        // TODO: check if we need to delete
        // Compactor(Compactor&&) = delete;
        // Compactor& operator=(Compactor&&) = delete;

        /**
         * @brief Compacts a single source file into existing destination files.
         *
         * This function is designed for N-to-1 merge compaction:
         * - The caller opens destination files ONCE
         * - Calls this function N times (once per source file)
         * - The caller closes destination files after all compactions
         *
         * Process per source file:
         * 1. Opens source file for reading
         * 2. Streams through source, filtering live entries
         * 3. Appends live entries to destination files (via O_APPEND)
         * 4. Updates KeyDir with new locations
         * 5. Closes source file and deletes it
         *
         * NOTE: This function does NOT sync or close destination files.
         * The caller is responsible for fsync() and close() after all
         * source files have been processed.
         *
         * @param src_file_id The source file ID to compact
         * @param dst_data_fd Destination data file descriptor (opened with O_APPEND)
         * @param dst_hint_fd Destination hint file descriptor (opened with O_APPEND)
         * @param dst_file_id The destination file ID (for KeyDir updates)
         * @return FileCompactionStats on success, or an error
         */
        kio::Task<kio::Result<Stats>> compact_one(uint64_t src_file_id, int dst_data_fd, int dst_hint_fd, uint64_t dst_file_id);

        /**
         * @brief Checks if an entry is still live (referenced by KeyDir).
         */
        [[nodiscard]] bool is_live_entry(const DataEntry& entry, uint64_t old_offset, uint64_t old_file_id) const;

        /**
         * @brief Gets the current write position for the destination file.
         *
         * Useful for the caller to track progress across multiple compactions.
         */
        [[nodiscard]] uint64_t get_current_data_offset() const { return current_data_offset_; }

        /**
         * @brief Resets internal state for a new compaction run.
         *
         * Call this before starting a new N-to-1 merge to reset offsets.
         */
        void reset_for_new_merge();

    private:
        struct KeyDirUpdate
        {
            std::string key;
            ValueLocation new_loc;
            uint64_t expected_file_id;
            uint64_t expected_offset;
        };

        kio::io::Worker& io_worker_;
        BitcaskConfig& config_;
        KeyDir& keydir_;
        CompactionLimits limits_;

        // Buffers (reused across compactions)
        std::vector<char> decode_buffer_;
        std::vector<char> data_batch_;
        std::vector<char> hint_batch_;
        std::vector<KeyDirUpdate> keydir_batch_;

        // Track position in a destination file (persists across compact_one calls)
        uint64_t current_data_offset_;
        uint64_t current_hint_offset_;

        kio::Task<kio::Result<std::pair<int, uint64_t>>> open_source_file(uint64_t src_file_id) const;

        void process_parsed_entry(const DataEntry& data_entry, uint64_t decoded_size, uint64_t entry_absolute_offset, uint64_t src_file_id, uint64_t dst_file_id, size_t window_start, Stats& stats);

        kio::Task<kio::Result<void>> parse_entries_from_buffer(uint64_t file_read_pos, uint64_t src_file_id, uint64_t dst_file_id, int dst_data_fd, int dst_hint_fd, size_t& window_start,
                                                               Stats& stats);

        void compact_decode_buffer(size_t& window_start);

        kio::Task<kio::Result<void>> stream_and_compact_file(int src_fd, int dst_data_fd, int dst_hint_fd, uint64_t src_file_id, uint64_t dst_file_id, Stats& stats);

        kio::Task<kio::Result<void>> commit_batch(int dst_data_fd, int dst_hint_fd, Stats& stats);

        [[nodiscard]] bool should_flush_batch() const;
        void reset_batches();
        kio::Task<kio::Result<void>> cleanup_source_files(uint64_t src_file_id) const;
    };
}  // namespace bitcask

#endif  // KIO_COMPACTION_H
