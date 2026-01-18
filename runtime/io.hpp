#pragma once
#include "executor.hpp"
#include <span>
#include <expected>

namespace uring::io
{
////////////////////////////////////////////////////////////////////////////////
// High Level API (Facade with Type Safety)
////////////////////////////////////////////////////////////////////////////////

// --- Concept Check for SocketAddress ---
// We don't include net.hpp, but we enforce the shape of the type.
template <typename T>
concept SocketAddressable = requires(const T& t) {
    { t.get() } -> std::convertible_to<const sockaddr*>;
    { t.addrlen } -> std::convertible_to<socklen_t>;
};

/// @brief Asynchronously reads from a file descriptor.
/// @param ex The executor managing the I/O.
/// @param fd The file descriptor to read from.
/// @param buf The buffer to read into.
/// @param off The offset to read from (defaults to 0).
/// @return A Result containing the number of bytes read.
/// @note The buffer `buf` must remain valid until the operation completes (i.e., co_await resumes).
inline auto read(Executor& ex, int fd, std::span<char> buf, uint64_t off = 0)
{
    return ReadOp(ex, fd, buf.data(), buf.size_bytes(), off);
}

/// @brief Asynchronously writes to a file descriptor.
/// @param ex The executor managing the I/O.
/// @param fd The file descriptor to write to.
/// @param buf The buffer containing data to write.
/// @param off The offset to write to (defaults to 0).
/// @return A Result containing the number of bytes written.
/// @note The buffer `buf` must remain valid until the operation completes (i.e., co_await resumes).
inline auto write(Executor& ex, int fd, std::span<const char> buf, uint64_t off = 0)
{
    return WriteOp(ex, fd, buf.data(), buf.size_bytes(), off);
}

/// @brief Asynchronously flushes changes to disk.
/// @param ex The executor.
/// @param fd The file descriptor to sync.
/// @param datasync If true, uses fdatasync (data only); otherwise fsync (data + metadata).
/// @return A Result indicating success or failure.
inline auto fsync(Executor& ex, int fd, bool datasync = false)
{
    return FsyncOp(ex, fd, datasync);
}

/// @brief Asynchronously accepts a new connection on a listening socket.
/// @param ex The executor.
/// @param fd The listening socket file descriptor.
/// @param addr Optional pointer to store the peer address.
/// @param len Optional pointer to store/update the address length.
/// @return A Result containing the new connected file descriptor.
inline auto accept(Executor& ex, int fd, sockaddr* addr = nullptr, socklen_t* len = nullptr)
{
    return AcceptOp(ex, fd, addr, len);
}

/// @brief Asynchronously connects to a remote address.
/// @param ex The executor.
/// @param fd The socket file descriptor.
/// @param addr The destination address.
/// @param len The length of the address structure.
/// @return A Result indicating success or failure.
inline auto connect(Executor& ex, int fd, const sockaddr* addr, socklen_t len)
{
    return ConnectOp(ex, fd, addr, len);
}

/// @brief Asynchronously connects to a remote address (SocketAddress overload).
/// @tparam T A type satisfying the SocketAddressable concept.
/// @param ex The executor.
/// @param fd The socket file descriptor.
/// @param addr The SocketAddress object.
/// @return A Result indicating success or failure.
template <SocketAddressable T>
auto connect(Executor& ex, int fd, const T& addr)
{
    return ConnectOp(ex, fd, addr.get(), addr.addrlen);
}

/// @brief Asynchronously receives data from a socket.
/// @param ex The executor.
/// @param fd The socket file descriptor.
/// @param buf The buffer to store received data.
/// @param flags Socket flags (e.g., MSG_WAITALL).
/// @return A Result containing the number of bytes received.
/// @note The buffer `buf` must remain valid until the operation completes.
inline auto recv(Executor& ex, int fd, std::span<char> buf, int flags = 0)
{
    return RecvOp(ex, fd, buf.data(), buf.size_bytes(), flags);
}

/// @brief Asynchronously sends data to a socket.
/// @param ex The executor.
/// @param fd The socket file descriptor.
/// @param buf The buffer containing data to send.
/// @param flags Socket flags (e.g., MSG_MORE).
/// @return A Result containing the number of bytes sent.
/// @note The buffer `buf` must remain valid until the operation completes.
inline auto send(Executor& ex, int fd, std::span<const char> buf, int flags = 0)
{
    return SendOp(ex, fd, buf.data(), buf.size_bytes(), flags);
}

/// @brief Asynchronously closes a file descriptor.
/// @param ex The executor.
/// @param fd The file descriptor to close.
/// @return A Result indicating success or failure.
inline auto close(Executor& ex, int fd)
{
    return CloseOp(ex, fd);
}

/// @brief Asynchronously sleeps for a specified duration.
/// @tparam Rep The representation type of the duration.
/// @tparam Period The period type of the duration.
/// @param ex The executor.
/// @param d The duration to sleep (std::chrono duration).
/// @return A TimeoutOp that can be co_awaited.
template <typename Rep, typename Period>
auto timeout(Executor& ex, std::chrono::duration<Rep, Period> d)
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    __kernel_timespec ts{};
    ts.tv_sec = static_cast<int64_t>(ns / 1'000'000'000ULL);
    ts.tv_nsec = static_cast<long long>(ns % 1'000'000'000ULL);
    return TimeoutOp(ex, ts);
}

/// @brief Asynchronously sleeps for a specified number of milliseconds.
/// @param ex The executor.
/// @param ms The number of milliseconds to sleep.
/// @return A TimeoutOp that can be co_awaited.
inline auto timeout_ms(Executor& ex, uint64_t ms)
{
    return timeout(ex, std::chrono::milliseconds(ms));
}

/// @brief Asynchronously cancels a pending operation by its user_data pointer.
/// @param ex The executor.
/// @param target The user_data pointer of the operation to cancel.
/// @return A Result indicating success or failure.
inline auto cancel(Executor& ex, void* target)
{
    return CancelOp(ex, target);
}

/// @brief Asynchronously cancels all pending operations for a specific file descriptor.
/// @param ex The executor.
/// @param fd The file descriptor to cancel operations for.
/// @return A Result indicating the number of cancelled operations.
inline auto cancel_fd(Executor& ex, const int fd)
{
    return CancelFdOp(ex, fd);
}

/// @brief Asynchronously reads using scattered buffers (vectorized I/O).
/// @param ex The executor.
/// @param fd The file descriptor.
/// @param iov Array of iovec structures.
/// @param iovcnt Number of iovec structures.
/// @param offset Offset to read from (default -1 for current position).
/// @return A Result containing the number of bytes read.
/// @note The `iov` array and all referenced buffers must remain valid until completion.
inline auto readv(Executor& ex, int fd, const iovec* iov, int iovcnt, uint64_t offset = static_cast<uint64_t>(-1))
{
    return ReadvOp(ex, fd, iov, iovcnt, offset);
}

/// @brief Asynchronously writes using gathered buffers (vectorized I/O).
/// @param ex The executor.
/// @param fd The file descriptor.
/// @param iov Array of iovec structures.
/// @param iovcnt Number of iovec structures.
/// @param offset Offset to write to (default -1 for current position).
/// @return A Result containing the number of bytes written.
/// @note The `iov` array and all referenced buffers must remain valid until completion.
inline auto writev(Executor& ex, int fd, const iovec* iov, int iovcnt, uint64_t offset = static_cast<uint64_t>(-1))
{
    return WritevOp(ex, fd, iov, iovcnt, offset);
}

/// @brief Asynchronously polls a file descriptor for events.
/// @param ex The executor.
/// @param fd The file descriptor.
/// @param events The events to poll for (e.g., POLLIN, POLLOUT).
/// @return A Result containing the resulting events mask.
inline auto poll(Executor& ex, int fd, int events)
{
    return PollOp(ex, fd, events);
}

/// @brief Asynchronously sends a message on a socket using a message header.
/// @param ex The executor.
/// @param fd The socket file descriptor.
/// @param msg The message header structure.
/// @param flags Send flags.
/// @return A Result containing the number of bytes sent.
/// @note The `msg` structure and all its pointed data must remain valid until completion.
inline auto sendmsg(Executor& ex, int fd, const msghdr* msg, int flags = 0)
{
    return SendmsgOp(ex, fd, msg, flags);
}

/// @brief Asynchronously receives a message on a socket using a message header.
/// @param ex The executor.
/// @param fd The socket file descriptor.
/// @param msg The message header structure.
/// @param flags Receive flags.
/// @return A Result containing the number of bytes received.
/// @note The `msg` structure and all its pointed buffers must remain valid until completion.
inline auto recvmsg(Executor& ex, int fd, msghdr* msg, int flags = 0)
{
    return RecvmsgOp(ex, fd, msg, flags);
}

/// @brief Asynchronously opens a file relative to the current working directory.
/// @param ex The executor.
/// @param path The path to the file.
/// @param flags Open flags (e.g., O_RDONLY, O_CREAT).
/// @param mode File permissions to use if creating a new file.
/// @return A Result containing the new file descriptor.
inline auto openat(Executor& ex, std::filesystem::path path, int flags, mode_t mode = 0)
{
    return OpenAtOp(ex, AT_FDCWD, std::move(path), flags, mode);
}

/// @brief Asynchronously opens a file relative to a directory file descriptor.
/// @param ex The executor.
/// @param dirfd The directory file descriptor.
/// @param path The path relative to dirfd.
/// @param flags Open flags.
/// @param mode File permissions.
/// @return A Result containing the new file descriptor.
inline auto openat(Executor& ex, int dirfd, std::filesystem::path path, int flags, mode_t mode = 0)
{
    return OpenAtOp(ex, dirfd, std::move(path), flags, mode);
}

/// @brief Asynchronously removes a file relative to the current working directory.
/// @param ex The executor.
/// @param path The path to the file.
/// @param flags Removal flags (e.g., AT_REMOVEDIR).
/// @return A Result indicating success or failure.
inline auto unlinkat(Executor& ex, std::filesystem::path path, int flags = 0)
{
    return UnlinkAtOp(ex, AT_FDCWD, std::move(path), flags);
}

/// @brief Asynchronously removes a file relative to a directory file descriptor.
/// @param ex The executor.
/// @param dirfd The directory file descriptor.
/// @param path The path relative to dirfd.
/// @param flags Removal flags.
/// @return A Result indicating success or failure.
inline auto unlinkat(Executor& ex, int dirfd, std::filesystem::path path, int flags = 0)
{
    return UnlinkAtOp(ex, dirfd, std::move(path), flags);
}

/// @brief Asynchronously preallocates or deallocates space for a file.
/// @param ex The executor.
/// @param fd The file descriptor.
/// @param mode Allocation mode (e.g., FALLOC_FL_KEEP_SIZE).
/// @param size The size to allocate.
/// @return A Result indicating success or failure.
inline auto fallocate(Executor& ex, int fd, int mode, off_t size)
{
    return FallocateOp(ex, fd, mode, 0, size);
}

/// @brief Asynchronously preallocates or deallocates space for a file range.
/// @param ex The executor.
/// @param fd The file descriptor.
/// @param mode Allocation mode.
/// @param offset The starting offset.
/// @param len The length of the range.
/// @return A Result indicating success or failure.
inline auto fallocate(Executor& ex, int fd, int mode, off_t offset, off_t len)
{
    return FallocateOp(ex, fd, mode, offset, len);
}

/// @brief Asynchronously truncates a file to a specified length.
/// @param ex The executor.
/// @param fd The file descriptor.
/// @param length The new file size.
/// @return A Result indicating success or failure.
inline auto ftruncate(Executor& ex, int fd, off_t length)
{
    return FtruncateOp(ex, fd, length);
}

/// @brief Asynchronously moves data between two file descriptors (zero-copy).
/// @param ex The executor.
/// @param fd_in The source file descriptor.
/// @param off_in The source offset (-1 for current).
/// @param fd_out The destination file descriptor.
/// @param off_out The destination offset (-1 for current).
/// @param len The number of bytes to copy.
/// @param flags Splice flags.
/// @return A Result containing the number of bytes spliced.
inline auto splice(Executor& ex, int fd_in, off_t off_in, int fd_out, off_t off_out, unsigned int len,
                   unsigned int flags = 0)
{
    return SpliceOp(ex, fd_in, off_in, fd_out, off_out, len, flags);
}

////////////////////////////////////////////////////////////////////////////////
// Helper Functions - Exact Read/Write
////////////////////////////////////////////////////////////////////////////////

/// @brief Helper to read exactly the requested number of bytes, looping if necessary.
/// @param exec The executor.
/// @param fd The file descriptor.
/// @param buf The buffer to fill.
/// @return Result<void> on success, or error (including EPIPE on early EOF).
/// @note The buffer `buf` must remain valid until completion.
inline Task<Result<void>> read_exact(Executor& exec, int fd, std::span<char> buf)
{
    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (total_bytes_read < total_to_read)
    {
        auto res = co_await read(exec, fd, buf.subspan(total_bytes_read), static_cast<uint64_t>(-1));
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_read = *res;
        if (bytes_read == 0)
            co_return error_from_errno(EPIPE);  // EOF before reading complete

        total_bytes_read += static_cast<size_t>(bytes_read);
    }

    co_return {};
}

/// @brief Helper to read exactly bytes at a specific offset, looping if necessary.
/// @param exec The executor.
/// @param fd The file descriptor.
/// @param buf The buffer to fill.
/// @param offset The starting file offset.
/// @return Result<void> on success, or error.
/// @note The buffer `buf` must remain valid until completion.
inline Task<Result<void>> read_exact_at(Executor& exec, int fd, std::span<char> buf, uint64_t offset)
{
    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (total_bytes_read < total_to_read)
    {
        const uint64_t current_offset = offset + total_bytes_read;
        std::span<char> remaining_buf = buf.subspan(total_bytes_read);

        auto res = co_await read(exec, fd, remaining_buf, current_offset);
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_read = *res;
        if (bytes_read == 0)
            co_return error_from_errno(EPIPE);  // EOF before reading complete

        total_bytes_read += static_cast<size_t>(bytes_read);
    }

    co_return {};
}

/// @brief Helper to write exactly the requested number of bytes, looping if necessary.
/// @param exec The executor.
/// @param fd The file descriptor.
/// @param buf The buffer to write.
/// @return Result<void> on success, or error.
/// @note The buffer `buf` must remain valid until completion.
inline Task<Result<void>> write_exact(Executor& exec, int fd, std::span<const char> buf)
{
    size_t total_bytes_written = 0;
    const size_t total_to_write = buf.size();

    while (total_bytes_written < total_to_write)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);

        auto res = co_await write(exec, fd, remaining_buf, static_cast<uint64_t>(-1));
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_written = *res;
        if (bytes_written == 0)
            co_return error_from_errno(EPIPE);  // EOF/error before writing complete

        total_bytes_written += static_cast<size_t>(bytes_written);
    }

    co_return {};
}

/// @brief Helper to write exactly bytes at a specific offset, looping if necessary.
/// @param exec The executor.
/// @param fd The file descriptor.
/// @param buf The buffer to write.
/// @param offset The starting file offset.
/// @return Result<void> on success, or error.
/// @note The buffer `buf` must remain valid until completion.
inline Task<Result<void>> write_exact_at(Executor& exec, int fd, std::span<const char> buf, uint64_t offset)
{
    size_t total_bytes_written = 0;
    const size_t total_to_write = buf.size();

    while (total_bytes_written < total_to_write)
    {
        std::span<const char> remaining_buf = buf.subspan(total_bytes_written);
        const uint64_t current_offset = offset + total_bytes_written;

        auto res = co_await write(exec, fd, remaining_buf, current_offset);
        if (!res)
            co_return std::unexpected(res.error());

        const int bytes_written = *res;
        if (bytes_written == 0)
            co_return error_from_errno(EPIPE);  // EOF/error before writing complete

        total_bytes_written += static_cast<size_t>(bytes_written);
    }

    co_return {};
}

/// @brief High-performance zero-copy file transfer using cached pipes and splice.
/// @param exec The executor.
/// @param out_fd The destination file descriptor (e.g., socket).
/// @param in_fd The source file descriptor (e.g., file on disk).
/// @param offset The starting offset in the source file.
/// @param count The number of bytes to transfer.
/// @return Result<void> on success.
/// @note Reduces syscalls by reusing thread-local pipes.
inline Task<Result<void>> sendfile(Executor& exec, int out_fd, int in_fd, off_t offset, size_t count)
{
    // ... existing code ...
    auto pipe_lease = detail::SplicePipePool::acquire();
    if (!pipe_lease)
        co_return std::unexpected(pipe_lease.error());

    const int pipe_rd = pipe_lease->read_fd;
    const int pipe_wr = pipe_lease->write_fd;

    size_t remaining = count;
    off_t current_offset = offset;

    while (remaining > 0)
    {
        // Default pipe buffer on Linux is usually 64KB (16 pages)
        constexpr size_t chunk_size = 65536;
        const size_t to_splice = std::min(remaining, chunk_size);

        // 2. Splice from FILE -> PIPE
        auto res_in = co_await splice(exec, in_fd, current_offset, pipe_wr, static_cast<off_t>(-1),
                                      static_cast<unsigned int>(to_splice), 0);
        if (!res_in)
            co_return std::unexpected(res_in.error());

        const int bytes_in = *res_in;
        if (bytes_in == 0)
            co_return error_from_errno(EPIPE); // EOF unexpected here

        // 3. Splice from PIPE -> SOCKET/FILE
        // We must loop here because the pipe might be full or the socket might not accept all data at once
        auto pipe_remaining = static_cast<size_t>(bytes_in);
        while (pipe_remaining > 0)
        {
            auto res_out = co_await splice(exec, pipe_rd, static_cast<off_t>(-1), out_fd, static_cast<off_t>(-1),
                                           static_cast<unsigned int>(pipe_remaining), 0);
            if (!res_out)
                co_return std::unexpected(res_out.error());

            const int bytes_out = *res_out;
            if (bytes_out == 0)
                co_return error_from_errno(EPIPE);

            pipe_remaining -= static_cast<size_t>(bytes_out);
        }

        current_offset += bytes_in;
        remaining -= bytes_in;
    }

    // Lease destructor automatically returns the pipe to the pool here
    co_return {};
}
}  // namespace uring::io