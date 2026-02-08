#pragma once

#include "stats.hpp"
#include "bitcask/common.hpp"
#include "bitcask/files.hpp"

namespace bitcask
{
    // ===================================================================
    // PartitionIO - Core KV Operations + File Management
    // ===================================================================
    class PartitionIO
    {
    public:
        PartitionIO(const BitcaskConfig& config, size_t partition_id, PartitionStats& stats);

        // Core operations
        kio::Task<kio::Result<void>> Put(kio::IoContext& ctx, std::string key, std::span<const std::byte> value);
        kio::Task<kio::Result<std::optional<std::vector<std::byte>>>> Get(kio::IoContext& ctx, std::string_view key);
        kio::Task<kio::Result<void>> Del(kio::IoContext& ctx, std::string key);

        // File lifecycle
        kio::Task<kio::Result<void>> RotateActiveFile(kio::IoContext& ctx);
        kio::Task<kio::Result<void>> SealActiveFile(kio::IoContext& ctx);
        kio::Task<kio::Result<void>> WriteHintFile(kio::IoContext& ctx, uint64_t file_id);

        // Access for compaction
        KeyDir& GetKeyDir() { return keydir_; }
        const KeyDir& GetKeyDir() const { return keydir_; }
        FDCache& GetFDCache() { return fd_cache_; }
        std::vector<uint64_t> ScanDataFiles() const;
        size_t PartitionID() const { return partition_id_; }
        FileIdGenerator& FdGen() { return file_id_gen_; }

        std::optional<uint64_t> ActiveFileID() const
        {
            if (active_file_ == nullptr)
            {
                return std::nullopt;
            }
            return active_file_->FileId();
        }

    private:
        KeyDir keydir_;
        PartitionStats& stats_;
        std::unique_ptr<DataFile> active_file_;
        FDCache fd_cache_;
        FileIdGenerator file_id_gen_;
        BitcaskConfig config_;
        size_t partition_id_;


        kio::Task<kio::Result<void>> CreateAndSetActiveFile(kio::IoContext& ctx);

        std::filesystem::path GetDataFilePath(uint64_t file_id) const
        {
            return config_.directory / std::format("partition_{}/data_{}.db", partition_id_, file_id);
        }

        std::filesystem::path GetHintFilePath(uint64_t file_id) const
        {
            return config_.directory / std::format("partition_{}/hint_{}.ht", partition_id_, file_id);
        }

        [[nodiscard]] uint64_t ActiveFileId() const { return active_file_ ? active_file_->FileId() : 0; }
    };
}
