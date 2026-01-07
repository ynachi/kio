#include "kio/third_party/xxhash/xxhash.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <bitcask/include/bitcask.h>

using namespace bitcask;
using namespace kio;
using namespace kio::io;

BitKV::BitKV(const BitcaskConfig& db_config, const WorkerConfig& io_config, const size_t partition_count)
    : db_config_(db_config), io_config_(io_config), partitions_(partition_count), partition_count_(partition_count)
{
}

Task<Result<std::unique_ptr<BitKV>>> BitKV::Open(const BitcaskConfig& config, const WorkerConfig io_config,
                                                 size_t partition_count)
{
    ALOG_INFO("Opening BitKV with part count {}", partition_count);
    std::unique_ptr<BitKV> db(new BitKV(config, io_config, partition_count));

    try
    {
        // ensure dirs are created
        db->EnsureDirectories();
        // ensure the manifest is valid (must happen after ensure_directories)
        db->CheckOrCreateManifest();
    }
    catch (const std::exception& e)
    {
        ALOG_ERROR("Failed to initialize BitKV: {}", e.what());
        co_return std::unexpected(Error{ErrorCategory::kApplication, 1001});  // Generic error code for init fail
    }

    InitState state(partition_count);

    auto io_pool = std::make_unique<IOPool>(partition_count, io_config, [db_ptr = db.get(), &state](Worker& worker)
                                            { InitializePartition(*db_ptr, worker, state); });

    state.latch.wait();

    if (state.has_error)
    {
        io_pool->Stop();
        co_return std::unexpected(state.first_error.value());
    }

    ALOG_INFO("All {} partitions initialized successfully", partition_count);

    db->io_pool_ = std::move(io_pool);

    co_return std::move(db);
}

void BitKV::EnsureDirectories() const
{
    const auto kPerms = static_cast<std::filesystem::perms>(db_config_.dir_mode);

    try
    {
        // Create a base directory
        if (!std::filesystem::exists(db_config_.directory))
        {
            std::filesystem::create_directories(db_config_.directory);
            std::filesystem::permissions(db_config_.directory, kPerms);
        }

        // Create partition directories
        for (size_t i = 0; i < partition_count_; ++i)
        {
            if (auto partition_dir = db_config_.directory / std::format("partition_{}", i);
                !std::filesystem::exists(partition_dir))
            {
                std::filesystem::create_directories(partition_dir);
                std::filesystem::permissions(partition_dir, kPerms);
            }
        }
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        ALOG_ERROR("Failed to create directories: {}", e.what());
        throw;
    }
}

void BitKV::CheckOrCreateManifest() const
{
    if (std::filesystem::path manifest_path = db_config_.directory / kManifestFileName;
        std::filesystem::exists(manifest_path))
    {
        // Read and Verify
        std::ifstream file(manifest_path, std::ios::binary);
        if (!file.is_open())
        {
            ALOG_ERROR("Failed to open MANIFEST file");
            throw std::runtime_error("Cannot open MANIFEST");
        }

        std::vector buffer((std::istreambuf_iterator(file)), std::istreambuf_iterator<char>());
        auto res = struct_pack::deserialize<Manifest>(buffer);

        if (!res.has_value())
        {
            ALOG_ERROR("Corrupted MANIFEST file");
            throw std::runtime_error("Corrupted MANIFEST");
        }

        const auto& manifest = res.value();

        if (manifest.magic != kManifestMagic)
        {
            ALOG_ERROR("Invalid MANIFEST magic");
            throw std::runtime_error("Invalid MANIFEST magic");
        }

        if (manifest.partition_count != partition_count_)
        {
            ALOG_ERROR("Partition count mismatch! Config: {}, Manifest: {}", partition_count_,
                       manifest.partition_count);
            throw std::runtime_error("Partition count mismatch");
        }
    }
    else
    {
        // Create a new Manifest
        ALOG_INFO("Creating new MANIFEST with partition_count={}", partition_count_);
        Manifest manifest;
        manifest.partition_count = static_cast<uint32_t>(partition_count_);
        auto data = struct_pack::serialize(manifest);

        std::ofstream file(manifest_path, std::ios::binary);
        file.write(data.data(), static_cast<int>(data.size()));
        file.close();

        if (!file.good())
        {
            ALOG_ERROR("Failed to write MANIFEST");
            throw std::runtime_error("Failed to write MANIFEST");
        }
    }
}

DetachedTask BitKV::InitializePartition(BitKV& db, Worker& worker, InitState& state)
{
    size_t current_id = worker.GetId();
    auto res = co_await Partition::Open(db.db_config_, worker, current_id);

    if (!res)
    {
        ALOG_ERROR("Partition open failed, partition_id={}", current_id);
        // the detached task does not propagate errors or exception, so store the state
        state.has_error.store(true);
        {
            std::scoped_lock const lock(state.error_mutex);
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

Task<Result<void>> BitKV::Close() const
{
    for (const auto& partition : partitions_)
    {
        co_await partition->AsyncClose();
    }

    co_return {};
}

size_t BitKV::RouteToPartition(const std::string_view key) const
{
    // Use XXH3_64bits for stable, cross-platform routing
    return Hash(key) % partition_count_;
}

Partition& BitKV::GetPartition(const size_t partition_id) const
{
    assert(partition_id < partition_count_);
    return *partitions_.at(partition_id);
}

Task<Result<std::optional<std::vector<char>>>> BitKV::Get(const std::string& key) const
{
    co_return KIO_TRY(co_await GetPartition(RouteToPartition(key)).Get(key));
}

Task<Result<std::optional<std::string>>> BitKV::GetString(const std::string& key) const
{
    auto result = co_await Get(key);
    if (!result.has_value())
    {
        co_return std::unexpected(result.error());
    }

    if (!result.value().has_value())
    {
        co_return std::nullopt;
    }

    const auto& vec = result.value().value();
    co_return std::string(vec.begin(), vec.end());
}

Task<Result<void>> BitKV::Del(const std::string& key) const
{
    KIO_TRY(co_await GetPartition(RouteToPartition(key)).Del(key));
    co_return {};
}

Task<Result<void>> BitKV::Put(std::string&& key, std::vector<char>&& value) const
{
    KIO_TRY(co_await GetPartition(RouteToPartition(key)).Put(std::move(key), std::move(value)));
    co_return {};
}

Task<Result<void>> BitKV::Put(std::string key, std::string value) const
{
    KIO_TRY(co_await Put(std::move(key), std::vector(value.begin(), value.end())));
    co_return {};
}

Task<Result<void>> BitKV::Sync() const
{
    for (const auto& partition : partitions_)
    {
        KIO_TRY(co_await partition->Sync());
    }
    co_return {};
}

Task<Result<void>> BitKV::Compact() const
{
    for (const auto& partition : partitions_)
    {
        KIO_TRY(co_await partition->Compact());
    }
    co_return {};
}

BitKV::~BitKV()
{
    // Stop the worker pool
    // Partitions should already be closed via close()
    if (io_pool_)
    {
        io_pool_->Stop();
    }
}
