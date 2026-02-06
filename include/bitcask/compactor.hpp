#pragma once

#include "absl/container/inlined_vector.h"

#include "kio/kio.hpp"

#include <vector>

#include "files.hpp"
#include "bitcask/common.hpp"
#include "bitcask/entry.hpp"
#include "bitcask/stats.hpp"

namespace bitcask
{
    // TODO: parametrize those ?
    // Read Buffer: 64KB - 256KB (Large enough for standard entries)
    constexpr size_t kDefaultInBufReadSize = 256 * 1024;
    // Write Buffer: 4MB (Ideal for sequential disk writes)
    constexpr size_t kDefaultOutBufWriteSize = 4 * 1024 * 1024;

    struct CompactionResult
    {
        uint64_t bytes_reclaimed = 0;
        std::vector<HintEntry> new_hints; // To update KeyDir atomically later
    };

    struct CompactionLimits
    {
        size_t max_compact_size = 1024 * 1024 * 1024; // 1GB
        double min_fragmentation = 0.3;
    };


    /// Compact multiple files into one destination
    kio::Task<kio::Result<CompactionResult>> CompactFiles(
        BitcaskConfig& cfg,
        kio::IoContext& ctx,
        const std::vector<uint64_t>& src_file_ids,
        DataFile& dst_file,
        FDCache& fd_cache,
        const KeyDir& key_dir /* Read-only access to current index*/,
        uint64_t partition_id
    );
} // namespace bitcask
