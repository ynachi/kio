#pragma once
#include "kio/executor.hpp"
#include "kio/runtime.hpp"
#include <span>
#include <expected>

namespace kio::io
{
////////////////////////////////////////////////////////////////////////////////
// High Level API (Facade with Type Safety)
//
// All operations return implementation-specific Op types from the detail namespace.
// These are effectively opaque to the user who just co_awaits them.
////////////////////////////////////////////////////////////////////////////////

template <typename T>
concept SocketAddressable = requires(const T& t) {
    { t.get() } -> std::convertible_to<const sockaddr*>;
    { t.addrlen } -> std::convertible_to<socklen_t>;
};

/// @brief Helper to get the executor from the context.
/// Returns the internal Executor which is public inside detail.
inline detail::Executor& get_exec(ThreadContext& ctx) {
    return ctx.executor();
}

/// @brief Reads data from a file descriptor.
/// @param ctx The thread context to run on
/// @param fd File descriptor to read from
/// @param buf Buffer to read into. MUST remain valid until operation completes.
/// @param off Offset to read from (default: current position)
/// @return Awaitable that yields Result<int> with bytes read
///
/// @warning The buffer must remain valid until co_await returns!
/// @code
///   std::vector<char> buffer(1024);
///   auto result = co_await read(ctx, fd, buffer);  // OK
///
///   // WRONG - buffer destroyed before read completes:
///   auto op = read(ctx, fd, temp_buffer);
///   // temp_buffer destroyed here
///   co_await op;  // ⚠️ Undefined behavior
///   @endcode
inline auto read(ThreadContext& ctx, int fd, std::span<char> buf, uint64_t off = 0)
{
    return detail::ReadOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), off);
}

inline auto write(ThreadContext& ctx, int fd, std::span<const char> buf, uint64_t off = 0)
{
    return detail::WriteOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), off);
}

inline auto fsync(ThreadContext& ctx, int fd, bool datasync = false)
{
    return detail::FsyncOp(get_exec(ctx), fd, datasync);
}

inline auto accept(ThreadContext& ctx, int fd, sockaddr* addr = nullptr, socklen_t* len = nullptr)
{
    return detail::AcceptOp(get_exec(ctx), fd, addr, len);
}

inline auto connect(ThreadContext& ctx, int fd, const sockaddr* addr, socklen_t len)
{
    return detail::ConnectOp(get_exec(ctx), fd, addr, len);
}

template <SocketAddressable T>
auto connect(ThreadContext& ctx, int fd, const T& addr)
{
    return detail::ConnectOp(get_exec(ctx), fd, addr.get(), addr.addrlen);
}

inline auto recv(ThreadContext& ctx, int fd, std::span<char> buf, int flags = 0)
{
    return detail::RecvOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), flags);
}

inline auto send(ThreadContext& ctx, int fd, std::span<const char> buf, int flags = 0)
{
    return detail::SendOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), flags);
}

inline auto close(ThreadContext& ctx, int fd)
{
    return detail::CloseOp(get_exec(ctx), fd);
}

template <typename Rep, typename Period>
auto timeout(ThreadContext& ctx, std::chrono::duration<Rep, Period> d)
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    __kernel_timespec ts{};
    ts.tv_sec = static_cast<int64_t>(ns / 1'000'000'000ULL);
    ts.tv_nsec = static_cast<long long>(ns % 1'000'000'000ULL);
    return detail::TimeoutOp(get_exec(ctx), ts);
}

inline auto timeout_ms(ThreadContext& ctx, uint64_t ms)
{
    return timeout(ctx, std::chrono::milliseconds(ms));
}

inline auto cancel(ThreadContext& ctx, void* target)
{
    return detail::CancelOp(get_exec(ctx), target);
}

inline auto cancel_fd(ThreadContext& ctx, const int fd)
{
    return detail::CancelFdOp(get_exec(ctx), fd);
}

inline auto readv(ThreadContext& ctx, int fd, const iovec* iov, int iovcnt, uint64_t offset = static_cast<uint64_t>(-1))
{
    return detail::ReadvOp(get_exec(ctx), fd, iov, iovcnt, offset);
}

inline auto writev(ThreadContext& ctx, int fd, const iovec* iov, int iovcnt, uint64_t offset = static_cast<uint64_t>(-1))
{
    return detail::WritevOp(get_exec(ctx), fd, iov, iovcnt, offset);
}

inline auto poll(ThreadContext& ctx, int fd, int events)
{
    return detail::PollOp(get_exec(ctx), fd, events);
}

inline auto sendmsg(ThreadContext& ctx, int fd, const msghdr* msg, int flags = 0)
{
    return detail::SendmsgOp(get_exec(ctx), fd, msg, flags);
}

inline auto recvmsg(ThreadContext& ctx, int fd, msghdr* msg, int flags = 0)
{
    return detail::RecvmsgOp(get_exec(ctx), fd, msg, flags);
}

inline auto openat(ThreadContext& ctx, std::filesystem::path path, int flags, mode_t mode = 0)
{
    return detail::OpenAtOp(get_exec(ctx), AT_FDCWD, std::move(path), flags, mode);
}

inline auto openat(ThreadContext& ctx, int dirfd, std::filesystem::path path, int flags, mode_t mode = 0)
{
    return detail::OpenAtOp(get_exec(ctx), dirfd, std::move(path), flags, mode);
}

inline auto unlinkat(ThreadContext& ctx, std::filesystem::path path, int flags = 0)
{
    return detail::UnlinkAtOp(get_exec(ctx), AT_FDCWD, std::move(path), flags);
}

inline auto unlinkat(ThreadContext& ctx, int dirfd, std::filesystem::path path, int flags = 0)
{
    return detail::UnlinkAtOp(get_exec(ctx), dirfd, std::move(path), flags);
}

inline auto fallocate(ThreadContext& ctx, int fd, int mode, off_t size)
{
    return detail::FallocateOp(get_exec(ctx), fd, mode, 0, size);
}

inline auto fallocate(ThreadContext& ctx, int fd, int mode, off_t offset, off_t len)
{
    return detail::FallocateOp(get_exec(ctx), fd, mode, offset, len);
}

inline auto ftruncate(ThreadContext& ctx, int fd, off_t length)
{
    return detail::FtruncateOp(get_exec(ctx), fd, length);
}

inline auto splice(ThreadContext& ctx, int fd_in, off_t off_in, int fd_out, off_t off_out, unsigned int len,
                   unsigned int flags = 0)
{
    return detail::SpliceOp(get_exec(ctx), fd_in, off_in, fd_out, off_out, len, flags);
}

////////////////////////////////////////////////////////////////////////////////
// Helper Functions - Exact Read/Write
////////////////////////////////////////////////////////////////////////////////

inline Task<Result<void>> read_exact(ThreadContext& ctx, int fd, std::span<char> buf)
{
    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (total_bytes_read < total_to_read)
    {
        auto res = co_await read(ctx, fd, buf.subspan(total_bytes_read), static_cast<uint64_t>(-1));
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_read = *res;
        if (bytes_read == 0)
            co_return error_from_errno(EPIPE);

        total_bytes_read += static_cast<size_t>(bytes_read);
    }
    co_return {};
}

inline Task<Result<void>> read_exact_at(ThreadContext& ctx, int fd, std::span<char> buf, uint64_t offset)
{
    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (total_bytes_read < total_to_read)
    {
        const uint64_t current_offset = offset + total_bytes_read;
        std::span<char> remaining_buf = buf.subspan(total_bytes_read);

        auto res = co_await read(ctx, fd, remaining_buf, current_offset);
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_read = *res;
        if (bytes_read == 0)
            co_return error_from_errno(EPIPE);

        total_bytes_read += static_cast<size_t>(bytes_read);
    }
    co_return {};
}

inline Task<Result<void>> write_exact(ThreadContext& ctx, int fd, std::span<const char> buf)
{
    size_t total_bytes_written = 0;
    const size_t total_to_write = buf.size();

    while (total_bytes_written < total_to_write)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);

        auto res = co_await write(ctx, fd, remaining_buf, static_cast<uint64_t>(-1));
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_written = *res;
        if (bytes_written == 0)
            co_return error_from_errno(EPIPE);

        total_bytes_written += static_cast<size_t>(bytes_written);
    }
    co_return {};
}

inline Task<Result<void>> write_exact_at(ThreadContext& ctx, int fd, std::span<const char> buf, uint64_t offset)
{
    size_t total_bytes_written = 0;
    const size_t total_to_write = buf.size();

    while (total_bytes_written < total_to_write)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
        const uint64_t current_offset = offset + total_bytes_written;

        auto res = co_await write(ctx, fd, remaining_buf, current_offset);
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_written = *res;
        if (bytes_written == 0)
            co_return error_from_errno(EPIPE);

        total_bytes_written += static_cast<size_t>(bytes_written);
    }
    co_return {};
}

Task<Result<void>> sendfile(ThreadContext& ctx, int out_fd, int in_fd, off_t offset, size_t count);

}  // namespace kio::io