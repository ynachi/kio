#include <bitcask/include/bitcask.h>
#include <cstdlib>
#include <filesystem>
#include <mutex>

using namespace bitcask;
using namespace kio;
using namespace kio::io;

BitKV::BitKV(const BitcaskConfig& db_config, const WorkerConfig& io_config, const size_t partition_count) :
    db_config_(db_config), io_config_(io_config), partitions_(partition_count), partition_count_(partition_count)
{
}

Task<Result<std::unique_ptr<BitKV>>> BitKV::open(const BitcaskConfig& config, const WorkerConfig io_config, size_t partition_count)
{
    ALOG_INFO("Opening BitKV with part count {}", partition_count);
    std::unique_ptr<BitKV> db(new BitKV(config, io_config, partition_count));

    // ensure dirs are created and have the right permissions
    db->ensure_directories();

    InitState state(partition_count);

    auto io_pool = std::make_unique<IOPool>(partition_count, io_config, [db_ptr = db.get(), &state](Worker& worker) { initialize_partition(*db_ptr, worker, state).detach(); });

    state.latch.wait();

    // Check for errors
    if (state.has_error)
    {
        io_pool->stop();
        co_return std::unexpected(state.first_error.value());
    }

    ALOG_INFO("All {} partitions initialized successfully", partition_count);

    db->io_pool_ = std::move(io_pool);

    co_return std::move(db);
}

void BitKV::ensure_directories() const
{
    const auto perms = static_cast<std::filesystem::perms>(db_config_.dir_mode);

    try
    {
        // Create base directory
        if (!std::filesystem::exists(db_config_.directory))
        {
            std::filesystem::create_directories(db_config_.directory);
            std::filesystem::permissions(db_config_.directory, perms);
        }

        // Create partition directories
        for (size_t i = 0; i < partition_count_; ++i)
        {
            auto partition_dir = db_config_.directory / std::format("partition_{}", i);
            if (!std::filesystem::exists(partition_dir))
            {
                std::filesystem::create_directories(partition_dir);
                std::filesystem::permissions(partition_dir, perms);
            }
        }
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        ALOG_ERROR("Failed to create directories: {}", e.what());
        throw;
    }
}


DetachedTask BitKV::initialize_partition(BitKV& db, Worker& worker, InitState& state)
{
    size_t current_id = worker.get_id();
    auto res = co_await Partition::open(db.db_config_, worker, current_id);

    if (!res)
    {
        ALOG_ERROR("Partition open failed, partition_id={}", current_id);
        // detached task does not propagate errors or exception, so store the state
        state.has_error.store(true);
        {
            std::lock_guard lock(state.error_mutex);
            if (!state.first_error)
            {
                state.first_error = res.error();
            }
        }

        // Still count down!
        state.latch.count_down();
        co_return;
    }

    db.partitions_[current_id] = std::move(res.value());
    state.latch.count_down();
}

Task<Result<void>> BitKV::close()
{
    for (const auto& partition: partitions_)
    {
        co_await partition->async_close();
    }

    co_return {};
}

BitKV::~BitKV()
{
    // Stop the worker pool
    // This is safe to call from main thread (destructor always runs on main thread)
    // Partitions should already be closed via close()
    if (io_pool_)
    {
        io_pool_->stop();
    }
}
