#pragma once
#include "kio/io.hpp"

namespace kio::storage
{

struct OpenOptions
{
    int open_flags = O_RDONLY | O_CLOEXEC;
    mode_t mode = 0644;
    bool sync_on_write{false};
    bool cloexec{true};
};

class IStorage
{
public:
    virtual ~IStorage() = default;

    virtual Task<Result<FD>> Open(std::filesystem::path path, OpenOptions& opts) = 0;

    virtual Task<Result<void>> Close(FD& file) = 0;

    virtual Task<Result<size_t>> ReadAt(FD& file, std::span<std::byte> buffer, uint64_t offset) = 0;

    virtual Task<Result<size_t>> WriteAt(FD& file, std::span<const std::byte> buffer, uint64_t offset) = 0;

    virtual Task<Result<void>> Truncate(FD& file, uint64_t size) = 0;

    virtual Task<Result<void>> Allocate(FD& file, uint64_t offset, uint64_t len) = 0;

    virtual Task<Result<void>> Fsync(FD& file) = 0;
    virtual Task<Result<void>> Fdatasync(FD& file) = 0;

    virtual Task<Result<void>> Mkdir(std::filesystem::path path) = 0;

    virtual Task<Result<void>> Rename(std::filesystem::path from, std::filesystem::path to) = 0;

    virtual Task<Result<void>> Unlink(std::filesystem::path path) = 0;

    virtual Task<Result<void>> FsyncDir(std::filesystem::path dir) = 0;

    virtual Task<Result<uint64_t>> FileSize(FD& file) = 0;
};

class StorageIO : public IStorage
{
    IoContext& ctx_;

public:
    explicit StorageIO(IoContext& ctx) : ctx_(ctx) {}

    Task<Result<FD>> Open(std::filesystem::path path, OpenOptions& opts) override;

    Task<Result<void>> Close(FD& file) override;

    Task<Result<size_t>> ReadAt(FD& file, std::span<std::byte> buffer, uint64_t offset) override;

    Task<Result<size_t>> WriteAt(FD& file, std::span<const std::byte> buffer, uint64_t offset) override;

    Task<Result<void>> Truncate(FD& file, uint64_t size) override;

    Task<Result<void>> Allocate(FD& file, uint64_t offset, uint64_t len) override;

    Task<Result<void>> Fsync(FD& file) override;
    Task<Result<void>> Fdatasync(FD& file) override;

    Task<Result<void>> Mkdir(std::filesystem::path path) override;

    Task<Result<void>> Rename(std::filesystem::path from, std::filesystem::path to) override;

    Task<Result<void>> Unlink(std::filesystem::path path) override;

    Task<Result<void>> FsyncDir(std::filesystem::path dir) override;

    Task<Result<uint64_t>> FileSize(FD& file) override;
};

}  // namespace kio::storage
