//
// Created by Yao ACHI on 06/10/2025.
//
#include "fs.h"

#include <filesystem>

#include "kio/core/async_logger.h"

namespace kio
{
namespace
{
std::string AbsolutePath(const std::string_view path)
{
    try
    {
        return std::filesystem::canonical(path).string();
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        return std::filesystem::absolute(path).string();
    }
}
}  // namespace
// always return an absolute path for hashing

FileManager::FileManager(const size_t io_worker_count, const io::WorkerConfig &config) : pool_(io_worker_count, config)
{
    ALOG_INFO("started file manager with {} workers", io_worker_count);
}

Task<Result<File> > FileManager::AsyncOpen(std::filesystem::path path, const int flags, const mode_t mode)
{
    auto worker_id = pool_.GetWorkerIdByKey(AbsolutePath(path.string()));
    auto *worker = pool_.GetWorker(worker_id);
    if (worker == nullptr)
    {
        // This should ideally not happen if pool is constructed correctly
        co_return std::unexpected(Error::FromErrno(EINVAL));
    }

    // Ensure we are on the correct worker thread before touching its ring.
    co_await io::SwitchToWorker(*worker);

    const int fd = KIO_TRY(co_await worker->AsyncOpenat(path, flags, mode));

    co_return std::expected<File, Error>(std::in_place, fd, pool_, worker_id);
}

Task<Result<size_t> > File::AsyncRead(std::span<char> buf, const uint64_t offset) const
{
    auto *worker = pool_.GetWorker(worker_id_);
    if (worker == nullptr)
    {
        co_return std::unexpected(Error::FromErrno(EINVAL));
    }

    // Switch to the assigned worker for this file descriptor.
    co_await io::SwitchToWorker(*worker);

    auto res = KIO_TRY(co_await worker->AsyncReadAt(fd_, buf, offset));

    co_return res;
}

// NOSONAR: buffer lifetime is controlled by caller, safe across suspend
Task<Result<size_t> > File::AsyncWrite(std::span<const char> buf, const uint64_t offset) const
{
    auto *worker = pool_.GetWorker(worker_id_);
    if (worker == nullptr)
    {
        co_return std::unexpected(Error::FromErrno(EINVAL));
    }

    // Switch to the assigned worker for this file descriptor.
    co_await io::SwitchToWorker(*worker);

    auto res = KIO_TRY(co_await worker->AsyncWriteAt(fd_, buf, offset));

    co_return res;
}
}  // namespace kio
