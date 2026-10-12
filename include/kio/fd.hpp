#pragma once
#include <utility>

#include <unistd.h>

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
            if (fd >= 0)
                ::close(fd);
        }

        static void noop_closer(int) noexcept
        {
        }

    private:
        int fd_ = -1;
        Closer closer_ = default_closer;

    public:
        UniqueFd() noexcept = default;

        explicit UniqueFd(const int fd, const Closer closer = default_closer) noexcept : fd_(fd), closer_(closer)
        {
        }

        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;

        UniqueFd(UniqueFd&& other) noexcept
            : fd_(std::exchange(other.fd_, -1)), closer_(std::exchange(other.closer_, default_closer))
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
            // Same fd: never close-and-rehold (that would store a closed fd
            // and double-close it later); just install the closer.
            if (fd_ == fd)
            {
                closer_ = closer;
                return;
            }
            if (fd_ >= 0 && closer_)
            {
                closer_(fd_);
            }
            fd_ = fd;
            closer_ = closer;
        }

        int release() noexcept
        {
            closer_ = default_closer;
            return std::exchange(fd_, -1);
        }

        /// Single option
        /// @code
        /// io::UniqueFd sock{raw_fd};
        /// if (auto r = sock.set_option(SockOpt::NoDelay{}); !r)
        ///    return r;
        /// @endcode
        template <SocketOption Opt>
        [[nodiscard]] Result<void> set_option(const Opt& opt) const noexcept
        {
            return opt.Apply(fd_);
        }

        /// Variadic — short-circuits on first error
        /// @code
        /// if (auto r = sock.set_options(
        ///     SockOpt::ReuseAddr{},
        ///     SockOpt::ReusePort{},
        ///     SockOpt::NonBlocking{},
        ///     SockOpt::RecvBuffer{256 * 1024},
        ///     SockOpt::SendBuffer{256 * 1024}); !r)
        /// return r;
        /// @endcode
        template <SocketOption... Opts>
        [[nodiscard]] Result<void> set_options(Opts&&... opts) const noexcept
        {
            Result<void> result;
            static_cast<void>(((result = std::forward<Opts>(opts).Apply(fd_), result.has_value()) && ...));
            return result;
        }
    };
} // namespace kio::io
