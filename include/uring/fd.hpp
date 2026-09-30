#pragma once
#include <utility>

#include <unistd.h>

namespace URing
{
struct Fd
{
    int fd = -1;
    Fd() = default;
    explicit Fd(int raw) noexcept : fd(raw) {}
    ~Fd()
    {
        if (fd >= 0)
            ::close(fd);
    }
    Fd(Fd&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other)
        {
            if (fd >= 0)
                ::close(fd);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int Get() const noexcept { return fd; }
    [[nodiscard]] bool IsValid() const noexcept { return fd >= 0; }
    [[nodiscard]] int Release() noexcept { return std::exchange(fd, -1); }
};
}  // namespace URing
