//
// Created by Yao ACHI on 06/10/2025.
//
#include "core/include/fs.h"

#include <filesystem>

#include "spdlog/spdlog.h"

namespace kio
{
    // always return an absolute path for hashing
    static std::string absolute_path(const std::string_view path)
    {
        try
        {
            return std::filesystem::canonical(path).string();
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            return std::filesystem::absolute(path).string();
        }
    }

    FileManager::FileManager(const size_t io_worker_count, const io::WorkerConfig& config) : pool_(io_worker_count, config) { spdlog::info("started file manager with {} workers", io_worker_count); }

    Task<std::expected<File, Error>> FileManager::async_open(std::string_view path, const int flags, const mode_t mode) noexcept
    {
        auto worker_id = pool_.get_worker_id_by_key(absolute_path(path));
        auto* worker = pool_.get_worker(worker_id);
        if (!worker)
        {
            // This should ideally not happen if pool is constructed correctly
            co_return std::unexpected(Error::from_errno(EINVAL));
        }

        // Ensure we are on the correct worker thread before touching its ring.
        co_await io::SwitchToWorker(*worker);

        const int maybe_fd = co_await worker->async_openat(path, flags, mode);

        if (maybe_fd < 0)
        {
            co_return std::unexpected(Error::from_errno(-maybe_fd));
        }

        co_return std::expected<File, Error>(std::in_place, maybe_fd, pool_, worker_id);
    }

    Task<std::expected<size_t, Error>> File::async_read(std::span<char> buf, const uint64_t offset) const
    {
        auto* worker = pool_.get_worker(worker_id_);
        if (!worker)
        {
            co_return std::unexpected(Error::from_errno(EINVAL));
        }

        // Switch to the assigned worker for this file descriptor.
        co_await io::SwitchToWorker(*worker);

        auto res = co_await worker->async_read(fd_, buf, offset);
        if (res < 0)
        {
            co_return std::unexpected(Error::from_errno(-res));
        }
        co_return res;
    }

    Task<std::expected<size_t, Error>> File::async_write(std::span<const char> buf, const uint64_t offset) const
    {
        auto* worker = pool_.get_worker(worker_id_);
        if (!worker)
        {
            co_return std::unexpected(Error::from_errno(EINVAL));
        }

        // Switch to the assigned worker for this file descriptor.
        co_await io::SwitchToWorker(*worker);

        auto res = co_await worker->async_write(fd_, buf, offset);
        if (res < 0)
        {
            co_return std::unexpected(Error::from_errno(-res));
        }
        co_return res;
    }

}  // namespace kio
