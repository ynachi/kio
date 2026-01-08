#ifndef KIO_BITCASK_COMPACTOR_H
#define KIO_BITCASK_COMPACTOR_H

#include "absl/container/inlined_vector.h"  // Import InlinedVector
#include "bitcask/include/common.h"
#include "bitcask/include/config.h"
#include "bitcask/include/stats.h"
#include "entry.h"
#include "kio/core/coro.h"
#include "kio/core/worker.h"

#include <cstdint>
#include <vector>

namespace bitcask::compactor
{

struct CompactionLimits
{
    size_t max_compact_size = 1024 * 1024 * 1024;  // 1GB
    double min_fragmentation = 0.3;
};

struct CompactionContext
{
    kio::io::Worker& worker;
    BitcaskConfig& config;
    KeyDir& keydir;
    PartitionStats& stats;
    size_t partition_id;
    uint64_t dst_file_id;

    // Optimization: Store up to 16 file IDs on the stack.
    // Most compactions involve fewer than 16 files.
    absl::InlinedVector<uint64_t, 16> src_file_ids;

    CompactionLimits limits;

    // Adjusted constructor to accept std::vector and move it into InlinedVector
    CompactionContext(kio::io::Worker& w, const BitcaskConfig& c, KeyDir& k, PartitionStats& s, size_t pid,
                      uint64_t dst, std::vector<uint64_t> src, CompactionLimits l)
        : worker(w), config(c), keydir(k), stats(s), partition_id(pid), dst_file_id(dst), limits(l)
    {
        src_file_ids.assign(src.begin(), src.end());
    }
};

kio::Task<kio::Result<void>> CompactFiles(CompactionContext& ctx);

}  // namespace bitcask::compactor

#endif  // KIO_BITCASK_COMPACTOR_H