#pragma once
#include <unistd.h>

#include "error.hpp"
#include "net.hpp"

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

    /// Single option
    /// @code
    /// URing::Fd sock{raw_fd};
    /// if (auto r = sock.SetOption(SockOpt::NoDelay{}); !r)
    ///    return r;
    /// @endcode
    template <SocketOption Opt>
    [[nodiscard]] Result<void> SetOption(const Opt& opt) const noexcept
    {
        return opt.Apply(fd);
    }

    /// Variadic — short-circuits on first error
    /// @code
    /// if (auto r = sock.SetOptions(
    ///     SockOpt::ReuseAddr{},
    ///     SockOpt::ReusePort{},
    ///     SockOpt::NonBlocking{},
    ///     SockOpt::RecvBuffer{256 * 1024},
    ///     SockOpt::SendBuffer{256 * 1024}); !r)
    /// return r;
    /// @endcode
    template <SocketOption... Opts>
    [[nodiscard]] Result<void> SetOptions(Opts&&... opts) const noexcept
    {
        Result<void> r;
        ((r = std::forward<Opts>(opts).Apply(fd), r.has_value()) && ...);
        return r;
    }
};
}  // namespace URing