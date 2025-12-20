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

    ~FileHandle() { close_fd(); }

    // Non-copyable
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    // Movable
    FileHandle(FileHandle&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    FileHandle& operator=(FileHandle&& other) noexcept
    {
        if (this != &other)
        {
            close_fd();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool is_valid() const { return fd_ >= 0; }

    void reset(const int new_fd = -1)
    {
        close_fd();
        fd_ = new_fd;
    }

    int release()
    {
        const int temp = fd_;
        fd_ = -1;
        return temp;
    }

private:
    int fd_;

    void close_fd()
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
