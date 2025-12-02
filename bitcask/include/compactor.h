//
// Created by Yao ACHI on 23/11/2025.
//

#ifndef KIO_COMPACTOR_H
#define KIO_COMPACTOR_H

#include "config.h"
#include "entry.h"
#include "kio/include/ds/bytes_mut.h"
#include "kio/include/io/worker.h"
#include "stats.h"

namespace bitcask::compactor
{
    struct CompactionLimits
    {
        size_t max_decode_buffer = 2 * 1024 * 1024;  // 2MB
        size_t max_hint_batch = 512 * 1024;
        size_t max_keydir_batch = 10000;
    };

    struct CompactionContext
    {
        kio::io::Worker& io_worker;
        const BitcaskConfig& config;
        const CompactionLimits& limits;
        uint64_t partition_id;

        SimpleKeydir& keydir;
        PartitionStats& stats;

        uint64_t dst_file_id;
        std::vector<uint64_t> src_file_ids;

        // Batching buffers
        std::vector<char> data_batch;
        std::vector<char> hint_batch;
        kio::BytesMut decode_buffer;

        struct KeyDirUpdate
        {
            std::string key;
            ValueLocation new_loc;
            uint64_t expected_file_id;
            uint64_t expected_offset;
        };
        std::vector<KeyDirUpdate> keydir_batch;

        uint64_t current_data_offset = 0;
        uint64_t current_hint_offset = 0;

        // Compaction metrics
        uint64_t entries_kept = 0;
        uint64_t entries_discarded = 0;
        uint64_t bytes_read = 0;
        uint64_t bytes_written = 0;

        explicit CompactionContext(kio::io::Worker& worker, const BitcaskConfig& cfg, SimpleKeydir& kd, PartitionStats& st, uint64_t partition, uint64_t dst_file, std::vector<uint64_t> src_files,
                                   const CompactionLimits& lim = {}) :
            io_worker(worker), config(cfg), limits(lim), partition_id(partition), keydir(kd), stats(st), dst_file_id(dst_file), src_file_ids(std::move(src_files)),
            decode_buffer(config.read_buffer_size * 2)
        {
            data_batch.reserve(config.write_buffer_size);
            hint_batch.reserve(limits.max_hint_batch);
            keydir_batch.reserve(limits.max_keydir_batch);
        }
    };

    // main public apis
    kio::Task<kio::Result<void>> compact_files(CompactionContext& ctx);
    kio::Task<kio::Result<void>> compact_one_file(CompactionContext& ctx, uint64_t src_file_id, int dst_data_fd, int dst_hint_fd);

    // Implementation helpers
    namespace detail
    {
        kio::Task<kio::Result<std::pair<int, uint64_t>>> open_source_file(const CompactionContext& ctx, uint64_t src_file_id);

        kio::Task<kio::Result<void>> stream_and_compact_file(CompactionContext& ctx, int src_fd, int dst_data_fd, int dst_hint_fd, uint64_t src_file_id);

        kio::Task<kio::Result<void>> parse_entries_from_buffer(CompactionContext& ctx, uint64_t file_read_pos, uint64_t src_file_id, int dst_data_fd, int dst_hint_fd);

        void process_parsed_entry(CompactionContext& ctx, const DataEntry& data_entry, uint64_t decoded_size, uint64_t entry_offset, uint64_t src_file_id, std::span<const char> readable_data);

        kio::Task<kio::Result<void>> commit_batch(CompactionContext& ctx, int dst_data_fd, int dst_hint_fd);

        bool should_flush_batch(const CompactionContext& ctx);
        void reset_batches(CompactionContext& ctx);

        kio::Task<kio::Result<void>> cleanup_source_files(const CompactionContext& ctx, uint64_t src_file_id);

        bool is_live_entry(const CompactionContext& ctx, const DataEntry& entry, uint64_t old_offset, uint64_t old_file_id);

        bool update_keydir_if_matches(const CompactionContext& ctx, const std::string& key, const ValueLocation& new_loc, uint64_t expected_file_id, uint64_t expected_offset);

        std::filesystem::path get_data_file_path(const CompactionContext& ctx, uint64_t file_id);

        std::filesystem::path get_hint_file_path(const CompactionContext& ctx, uint64_t file_id);
    }  // namespace detail
}  // namespace bitcask::compactor
#endif  // KIO_COMPACTOR_H
