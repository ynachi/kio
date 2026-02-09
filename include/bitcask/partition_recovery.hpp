#pragma once

#include "kio/core/core.hpp"

#include "partition_io.hpp"

namespace bitcask
{
kio::Task<kio::Result<void>> RecoverPartition(kio::IoContext& ctx, PartitionIO& io, PartitionStats& stats,
                                              const BitcaskConfig& config);

kio::Task<kio::Result<uint64_t>> RecoverFromDataFile(kio::IoContext& ctx, PartitionIO& io, const BitcaskConfig& config,
                                                     const kio::FDGuard& fh, uint64_t file_id);

kio::Task<kio::Result<uint64_t>> TryRecoverFromHint(kio::IoContext& ctx, PartitionIO& io, const BitcaskConfig& config,
                                                    uint64_t file_id);

kio::Task<kio::Result<uint64_t>> RecoverFromHintFile(kio::IoContext& ctx, PartitionIO& io, const kio::FDGuard& fh,
                                                     uint64_t file_id);
}  // namespace bitcask
