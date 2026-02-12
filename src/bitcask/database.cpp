#include "bitcask/database.hpp"

#include "kio/logger.hpp"

#include <filesystem>
#include <format>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <system_error>
#include <utility>

namespace bitcask
{
    namespace fs = std::filesystem;

    namespace
    {
        fs::path PartitionDirectory(const fs::path& root, const size_t partition_id)
        {
            return root / std::format("partition_{}", partition_id);
        }
    } // namespace

    BitKV::BitKV(BitcaskConfig db_cfg, const size_t partition_count)
        : db_config_(std::move(db_cfg)), partition_count_(partition_count)
    {
        partitions_.reserve(partition_count_);
    }

    BitKV::~BitKV()
    {
        if (!partitions_.empty())
        {
            ALOG_WARN("BitKV destroyed without calling Close(); {} partitions still open", partitions_.size());
        }
    }

    kio::Result<kio::IoContext*> BitKV::RequireCurrentContext()
    {
        auto* ctx = kio::this_context();
        if (ctx == nullptr)
        {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        return ctx;
    }

    kio::Task<kio::Result<std::unique_ptr<BitKV>>> BitKV::Open(BitcaskConfig config, size_t partition_count)
    {
        if (partition_count == 0)
        {
            partition_count = 1;
        }

        if (partition_count > std::numeric_limits<uint16_t>::max())
        {
            co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }

        try
        {
            config.Validate();
        }
        catch (const std::exception& ex)
        {
            ALOG_ERROR("BitKV::Open: invalid config: {}", ex.what());
            co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }

        std::error_code ec;
        fs::create_directories(config.directory, ec);
        if (ec)
        {
            co_return std::unexpected(ec);
        }

        auto db = std::unique_ptr<BitKV>(new BitKV(std::move(config), partition_count));
        auto* ctx = KIO_CO_TRY(RequireCurrentContext());
        auto rollback_opened_partitions = [&]() -> kio::Task<>
        {
            for (auto& opened_partition : db->partitions_)
            {
                auto close_res = co_await opened_partition->AsyncClose(*ctx);
                if (!close_res.has_value())
                {
                    ALOG_ERROR("BitKV::Open: failed to close partition {} during rollback: {}",
                               opened_partition->GetID(), close_res.error().message());
                }
            }
            db->partitions_.clear();
        };

        for (size_t partition_id = 0; partition_id < db->partition_count_; ++partition_id)
        {
            fs::create_directories(PartitionDirectory(db->db_config_.directory, partition_id), ec);
            if (ec)
            {
                co_await rollback_opened_partitions();
                co_return std::unexpected(ec);
            }

            auto partition_res = co_await Partition::AsyncOpen(*ctx, db->db_config_, partition_id);
            if (!partition_res.has_value())
            {
                co_await rollback_opened_partitions();
                co_return std::unexpected(partition_res.error());
            }

            db->partitions_.push_back(std::move(partition_res.value()));
        }

        co_return std::move(db);
    }

    uint32_t BitKV::RouteToPartition(const std::string_view key) const
    {
        return static_cast<uint32_t>(Hash(key) % partition_count_);
    }

    kio::Result<Partition*> BitKV::GetPartitionForKey(const std::string_view key)
    {
        if (partitions_.empty())
        {
            return std::unexpected(std::make_error_code(std::errc::state_not_recoverable));
        }

        const auto partition_id = RouteToPartition(key);
        if (partition_id >= partitions_.size())
        {
            return std::unexpected(std::make_error_code(std::errc::no_such_device));
        }

        auto* partition = partitions_[partition_id].get();
        if (partition == nullptr)
        {
            return std::unexpected(std::make_error_code(std::errc::no_such_device));
        }

        return partition;
    }

    kio::Task<kio::Result<void>> BitKV::Put(std::string key, std::vector<std::byte> value)
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }

        auto* ctx = KIO_CO_TRY(RequireCurrentContext());
        auto* partition = KIO_CO_TRY(GetPartitionForKey(key));
        KIO_CO_TRY(co_await partition->Put(*ctx, std::move(key), std::span<const std::byte>(value)));
        co_return {};
    }

    kio::Task<kio::Result<std::optional<std::vector<std::byte>>>> BitKV::Get(std::string_view key)
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }

        auto* ctx = KIO_CO_TRY(RequireCurrentContext());
        std::string key_copy(key);
        auto* partition = KIO_CO_TRY(GetPartitionForKey(key_copy));
        auto value = KIO_CO_TRY(co_await partition->Get(*ctx, key_copy));
        co_return value;
    }

    kio::Task<kio::Result<void>> BitKV::Del(std::string_view key)
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }

        auto* ctx = KIO_CO_TRY(RequireCurrentContext());
        std::string key_copy(key);
        auto* partition = KIO_CO_TRY(GetPartitionForKey(key_copy));
        KIO_CO_TRY(co_await partition->Del(*ctx, std::move(key_copy)));
        co_return {};
    }

    kio::Task<kio::Result<void>> BitKV::Sync()
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }

        // No explicit force-sync API exists at Partition level yet.
        // Durability is controlled by sync_on_write/background sync in PartitionIO.
        co_return {};
    }

    kio::Task<kio::Result<void>> BitKV::Compact()
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }

        auto* ctx = KIO_CO_TRY(RequireCurrentContext());
        for (auto& partition : partitions_)
        {
            KIO_CO_TRY(co_await partition->Compact(*ctx));
        }
        co_return {};
    }

    kio::Task<kio::Result<void>> BitKV::Close()
    {
        if (shutting_down_.exchange(true, std::memory_order_acq_rel))
        {
            co_return {};
        }

        auto* ctx = KIO_CO_TRY(RequireCurrentContext());
        std::optional<std::error_code> first_error;

        for (auto& partition : partitions_)
        {
            auto close_res = co_await partition->AsyncClose(*ctx);
            if (!close_res.has_value() && !first_error.has_value())
            {
                first_error = close_res.error();
            }
        }

        partitions_.clear();

        if (first_error.has_value())
        {
            co_return std::unexpected(*first_error);
        }

        co_return {};
    }
} // namespace bitcask
