//
// Created by Yao ACHI on 08/12/2025.
//

#ifndef KIO_NET_SOCKET_H
#define KIO_NET_SOCKET_H
#include <unistd.h>
#include <utility>

namespace kio::net
{
    /**
     * @brief A simple RAII wrapper for a file descriptor.
     * Closes the file descriptor on destruction unless released.
     */
    class Socket
    {
        int fd_ = -1;

    public:
        Socket() = default;

        explicit Socket(int fd) : fd_(fd) {}

        ~Socket()
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
            }
        }

        Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

        Socket& operator=(Socket&& other) noexcept
        {
            if (this != &other)
            {
                if (fd_ >= 0) ::close(fd_);
                fd_ = std::exchange(other.fd_, -1);
            }
            return *this;
        }

        // Disable copy
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;

        [[nodiscard]] int get() const { return fd_; }

        [[nodiscard]] bool is_valid() const { return fd_ >= 0; }

        explicit operator bool() const { return is_valid(); }

        /**
         * @brief Releases ownership of the file descriptor.
         * The caller is now responsible for closing it.
         * @return The file descriptor.
         */
        int release() { return std::exchange(fd_, -1); }
    };
}  // namespace kio::net
#endif  // KIO_NET_SOCKET_H
