//
// Created by Yao ACHI on 07/02/2026.
//


#include "kio/logger.hpp"
#include "bitcask/partition.hpp"

namespace bitcask
{
    kio::Task<kio::Result<std::unique_ptr<Partition>>> Partition::AsyncOpen(
        kio::IoContext& ctx, BitcaskConfig& config, size_t partition_id)
    {
        auto partition = std::unique_ptr<Partition>(new Partition(config, partition_id));

        KIO_CO_TRY(co_await RecoverPartition(ctx, partition->io_, partition->stats_, config));

        if (config.auto_compact)
        {
            partition->BgJobs().Spawn(partition->compactor_.CompactionLoop(ctx));
        }

        // Start the sync loop if needed
        if (!config.sync_on_write)
        {
            partition->BgJobs().Spawn(partition->io_.BackgroundSync(ctx));
        }

        co_return std::move(partition);
    }

    kio::Task<kio::Result<void>> Partition::AsyncClose(kio::IoContext& ctx)
    {
        compactor_.RequestStop();
        io_.RequestStop();

        // waits until the tasks exit naturally.
        co_await BgJobs().JoinAll(ctx);

        KIO_CO_TRY(co_await io_.SealActiveFile(ctx));

        co_return {};
    }
}
