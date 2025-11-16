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
    class Compactor
    {
        // compactor has a dedicated worker
        kio::io::Worker io_worker_;
        BitcaskConfig& config_;
        KeyDir& indexes_;
        // need a dedicated buffer pool as they are not thread safe
        kio::BufferPool bp;

        // create a new data and hint file
        kio::Task<kio::Result<std::pair<DataFile, HintFile>>> prep_compaction(uint64_t new_files_id);

    public:
        explicit Compactor(BitcaskConfig& config);
        ~Compactor();

        /// Compacts a single file
        // TODO: returns stat about the compaction
        kio::Task<kio::Result<void>> compact(uint64_t file_id);
    };
}  // namespace bitcask

#endif  // KIO_COMPACTION_H
