//
// Created by Yao ACHI on 19/11/2025.
//

#ifndef KIO_BITCASK_FILE_HANDLE_H
#define KIO_BITCASK_FILE_HANDLE_H

#include <unistd.h>

namespace bitcask
{
/**
 * @brief RAII wrapper for file descriptors.
 * Ensures file descriptors are closed when they go out of scope.
 */
class FileHandle
{
public:
    explicit FileHandle(int fd = -1) : fd_(fd) {}

    ~FileHandle() { CloseFd(); }

    // Non-copyable
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    // Movable
    FileHandle(FileHandle&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    FileHandle& operator=(FileHandle&& other) noexcept
    {
        if (this != &other)
        {
            CloseFd();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int Get() const { return fd_; }
    [[nodiscard]] bool IsValid() const { return fd_ >= 0; }

    void Reset(const int new_fd = -1)
    {
        CloseFd();
        fd_ = new_fd;
    }

    int Release()
    {
        const int kTemp = fd_;
        fd_ = -1;
        return kTemp;
    }

private:
    int fd_;

    void CloseFd()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }
};

}  // namespace bitcask

#endif  // KIO_BITCASK_FILE_HANDLE_H
