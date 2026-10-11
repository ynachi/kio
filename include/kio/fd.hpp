#pragma once
#include <unistd.h>
#include <utility>

#include "error.hpp"
#include "net.hpp"

namespace kio::io
{
    class UniqueFd
    {
    public:
        using Closer = void (*)(int) noexcept;

        static void default_closer(const int fd) noexcept
        {
            if (fd >= 0) ::close(fd);
        }

        static void noop_closer(int) noexcept
        {
        }

    private:
        int fd_ = -1;
        Closer closer_ = default_closer;

    public:
        UniqueFd() noexcept = default;

        explicit UniqueFd(const int fd, const Closer closer = default_closer) noexcept
            : fd_(fd), closer_(closer)
        {
        }

        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;

        UniqueFd(UniqueFd&& other) noexcept
            : fd_(std::exchange(other.fd_, -1)),
              closer_(std::exchange(other.closer_, default_closer))
        {
        }

        UniqueFd& operator=(UniqueFd&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                fd_ = std::exchange(other.fd_, -1);
                closer_ = std::exchange(other.closer_, default_closer);
            }
            return *this;
        }

        ~UniqueFd() { reset(); }

        [[nodiscard]] int get() const noexcept { return fd_; }

        [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

        void reset(const int fd = -1, Closer closer = default_closer) noexcept
        {
            if (fd_ == fd && closer_ == closer) return;
            if (fd_ >= 0 && closer_) closer_(fd_);
            fd_ = fd;
            closer_ = closer;
        }

        int release() noexcept
        {
            closer_ = default_closer;
            return std::exchange(fd_, -1);
        }
    };

    struct Fd
    {
        int fd = -1;

        Fd() = default;

        explicit Fd(const int f) : fd(f)
        {
        }

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
        [[nodiscard]] int Get() const noexcept { return fd; }

        /// @brief Check if the descriptor is valid.
        [[nodiscard]] bool IsValid() const noexcept { return fd >= 0; }

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
            Result<void> result;
            static_cast<void>(
                ((result = std::forward<Opts>(opts).Apply(fd), result.has_value()) && ...));
            return result;
        }
    };
} // namespace kio::io
