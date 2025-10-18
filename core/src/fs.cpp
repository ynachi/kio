//
// Created by Yao ACHI on 06/10/2025.
//
#include <filesystem>

#include "core/include/fs_demo.h"
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

     FileManager::FileManager(const size_t io_worker_count, const IoWorkerConfig& config):
         pool_(io_worker_count, config)
    {
         spdlog::info("started file manager with {} workers", io_worker_count);
     }

    Task<std::expected<File, IoError>> FileManager::async_open(std::string_view path, const int flags, const mode_t mode) noexcept
     {
        auto worker_id = pool_.get_io_worker_id_by_key(absolute_path(path));

         const int maybe_fd = co_await pool_.get_worker(worker_id)->async_openat(path, flags, mode);

         if (maybe_fd < 0)
         {
             co_return std::unexpected(IOErrorFromErno(-maybe_fd));
         }

         co_return std::expected<File, IoError>(std::in_place, maybe_fd, pool(), worker_id);
     }

    Task<std::expected<size_t, IoError>> File::async_read(std::span<char> buf, const uint64_t offset) const
    {
        auto res = co_await pool_.get_worker(worker_id_)->async_read(fd_, buf, offset);
        if (res < 0)
        {
            co_return std::unexpected(IOErrorFromErno(-res));
        }
        co_return res;
    }

    Task<std::expected<size_t, IoError>> File::async_write(std::span<const char> buf, const uint64_t offset) const
    {
        auto res = co_await pool_.get_worker(worker_id_)->async_write(fd_, buf, offset);
        if (res < 0)
        {
            co_return std::unexpected(IOErrorFromErno(-res));
        }
        co_return res;
    }

}