#pragma once
#include <unistd.h>

namespace URing
{
struct Fd
{
    int fd = -1;

    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    ~Fd()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
    }

    Fd(Fd&& other) noexcept : fd(other.fd) { other.fd = -1; }

    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other)
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    /// @brief Get the raw file descriptor.
    [[nodiscard]] int Get() const { return fd; }

    /// @brief Check if the descriptor is valid.
    [[nodiscard]] bool IsValid() const { return fd >= 0; }

    /// @brief Release ownership to the caller.
    [[nodiscard]] int Release() noexcept
    {
        const int out = fd;
        fd = -1;
        return out;
    }
};
}  // namespace URing