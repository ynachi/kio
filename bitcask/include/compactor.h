//
// Created by Yao ACHI on 14/11/2025.
//

#ifndef KIO_COMPACTION_H
#define KIO_COMPACTION_H
#include "config.h"
#include "core/include/io/worker.h"

using namespace kio;
using namespace kio::io;

namespace bitcask
{
    class Compactor
    {
        // compactor has a dedicated worker
        Worker io_worker_;
        BitcaskConfig& config_;

    public:
        Compactor(BitcaskConfig& config);
        ~Compactor();

        /// Compacts a single file
        Task<Result<void>> compact(uint64_t file_id);
    };
}  // namespace bitcask

#endif  // KIO_COMPACTION_H
