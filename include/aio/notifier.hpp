// aio/notifier.hpp
#pragma once

#include <system_error>
#include <utility>

#include <unistd.h>

#include <sys/eventfd.h>

#include "aio/io_context.hpp"

namespace aio
{

/// Thread-safe notification primitive.
///
/// Notifier allows a coroutine to wait for a signal from any thread.
/// It is designed for one-to-one signaling: a single Signal() call wakes up
/// exactly one waiting coroutine. This is NOT a broadcast mechanism (like
/// std::condition_variable::notify_all). If multiple coroutines wait on the
/// same Notifier, they will compete for signals.
///
/// It can be created without an IoContext, making it suitable for cross-thread
/// signaling where the IoContext might be created later or lives on a different
/// thread (e.g., inside a Worker).
///
/// @code
///   Notifier notifier;  // Created anywhere
///
///   // On worker thread:
///   co_await notifier.Wait(ctx);
///
///   // From any thread:
///   notifier.Signal();
/// @endcode
class Notifier
{
public:
    Notifier() : fd_(eventfd(0, EFD_CLOEXEC))
    {
        if (fd_ < 0)
        {
            throw std::system_error(errno, std::system_category(), "eventfd");
        }
    }

    ~Notifier()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    Notifier(Notifier&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    Notifier& operator=(Notifier&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    struct WaitOp : UringOp
    {
        int fd;
        uint64_t value{};

        WaitOp(IoContext* ctx, int f) : UringOp(ctx), fd(f) {}

        void PrepareSqe(io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, &value, sizeof(value), 0); }

        /// Returns the number of signals that were pending
        Result<uint64_t> await_resume()
        {
            if (res < 0)
            {
                return std::unexpected(MakeErrorCode(res));
            }
            return value;
        }
    };

    /// Wait for one or more signals. Takes IoContext as a parameter.
    [[nodiscard]] WaitOp Wait(IoContext& ctx) { return {&ctx, fd_}; }

    /// Signal the notifier (thread-safe). Wakes up one pending Wait().
    void Signal(uint64_t count = 1) const { [[maybe_unused]] auto r = ::write(fd_, &count, sizeof(count)); }

    [[nodiscard]] int Fd() const { return fd_; }

private:
    int fd_;
};

}  // namespace aio
