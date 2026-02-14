#include "bitcask/database.hpp"

#include "kio/logger.hpp"

#include <filesystem>
#include <format>
#include <exception>
#include <system_error>
#include <utility>

namespace bitcask
{
    namespace fs = std::filesystem;

    namespace
    {
        kio::Result<> EnsureDirectories(const BitcaskConfig& config) noexcept
        {
            const auto perms = static_cast<std::filesystem::perms>(config.dir_mode);
            std::error_code ec;

            // Create the main directory and set permissions
            std::filesystem::create_directories(config.directory, ec);
            if (ec) return std::unexpected(ec);

            // Enforce permissions even if it already existed (strict consistency)
            std::filesystem::permissions(config.directory, perms, ec);
            if (ec) return std::unexpected(ec);

            for (size_t i = 0; i < BitKV::kPartitionCount; ++i)
            {
                // Construct path: "db_root/partition_N"
                auto partition_dir = config.directory / std::format("partition_{}", i);

                std::filesystem::create_directories(partition_dir, ec);
                if (ec) return std::unexpected(ec);

                std::filesystem::permissions(partition_dir, perms, ec);
                if (ec) return std::unexpected(ec);
            }

            return {};
        }
    }

    BitKV::~BitKV()
    {
        if (!partitions_.empty())
        {
            ALOG_WARN("BitKV destroyed without calling Close(); {} partitions still open", partitions_.size());
        }
    }

    void BitKV::StartIoThreads(const size_t io_worker_count)
    {
        for (size_t i = 0; i < io_worker_count; ++i)
        {
            io_threads_.emplace_back([i, this]
            {
                PinToCpu(static_cast<int>(i));
                // SINGLE_ISSUER requires that the context is created in the thread
                auto ctx = std::make_unique<kio::IoContext>(config_.max_tasks_per_io_engine);
                // wait for the context to fully start
                ctx->WaitReady();
                // Save its pointer in the lookup table
                ctx_lookup_[i] = ctx.get();
                io_ctxs_[i] = std::move(ctx);
                // signal the DB this thread is fully started
                start_latch_.count_down();
                // we can now run the context
                ALOG_INFO("Io context {} created, now starting it", i);
                io_ctxs_[i]->Run();
                ALOG_INFO("Io context {} exited", i);
            });
        }
    }

    kio::Task<kio::Result<>> BitKV::CreatePartitions()
    {
        kio::TaskGroup<> tasks(kPartitionCount);

        for (size_t i = 0; i < kPartitionCount; ++i)
        {
            const size_t ctx_idx = i % io_worker_count_;
            kio::IoContext* target_ctx = io_ctxs_[ctx_idx].get();

            tasks.Spawn(
                [&, i, target_ctx]() -> kio::Task<>
                {
                    ALOG_INFO("Opening partition {} on io worker {}", i, ctx_idx);
                    co_await kio::SwitchTo(*target_ctx);

                    // Now we are on the worker thread. Safe to construct/open.
                    auto res = co_await Partition::Open(*target_ctx, config_, i);

                    if (!res.has_value())
                    {
                        //    If a partition is broken, the DB is broken.
                        //    Log fatal error and kill the process to prevent data corruption.
                        ALOG_ERROR("FATAL: Failed to open Partition {}: {}", i, res.error().message());
                        std::terminate();
                    }

                    partitions_[i] = std::move(*res);

                    co_return;
                }()
            );
        }

        auto* current_ctx = kio::IoContext::Current();
        co_await tasks.JoinAll(*current_ctx);

        co_return {};
    }

    kio::Task<kio::Result<std::unique_ptr<BitKV>>> BitKV::Open(BitcaskConfig& config, size_t io_worker_count)
    {
        KIO_CO_TRY(config.Validate());

        // config is good, now create directories
        KIO_CO_TRY(EnsureDirectories(config));
        auto db = std::unique_ptr<BitKV>(new BitKV(std::move(config), io_worker_count));

        // create io threads
        db->StartIoThreads(io_worker_count);

        db->start_latch_.wait();

        // create partitions
        KIO_CO_TRY_LOG(co_await db->CreatePartitions());

        db->BuildRoutes();

        ALOG_INFO("BitKV started successfully with {} partitions", kPartitionCount);
        co_return std::move(db);
    }

    void BitKV::BuildRoutes()
    {
        for (size_t i = 0; i < kPartitionCount; ++i)
        {
            const size_t ctx_idx = i % io_worker_count_;
            route_[i] = {.partition = partitions_[i].get(), .ctx = io_ctxs_[ctx_idx].get()};
        }
    }

    kio::Task<kio::Result<void>> BitKV::Put(std::string key, std::vector<std::byte> value) const
    {
        const auto route = Route(key);
        co_await kio::SwitchTo(route.ctx);
        co_return co_await route.partition.Put(route.ctx, std::move(key), std::move(value));
    }

    kio::Task<kio::Result<std::optional<std::vector<std::byte>>>> BitKV::Get(std::string_view key) const
    {
        const auto route = Route(key);
        co_await kio::SwitchTo(route.ctx);
        co_return co_await route.partition.Get(route.ctx, key);
    }

    kio::Task<kio::Result<void>> BitKV::Del(std::string key) const
    {
        const auto route = Route(key);
        co_await kio::SwitchTo(route.ctx);
        co_return co_await route.partition.Del(route.ctx, std::move(key));
    }
} // namespace bitcask
