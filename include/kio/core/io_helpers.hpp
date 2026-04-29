#pragma once

#include "kio/io.hpp"

#include <algorithm>
#include <cassert>
#include <span>

#include "core.hpp"

namespace kio
{

constexpr uint64_t kUseFilePos = std::numeric_limits<uint64_t>::max();

// -----------------------------------------------------------------------------
// Internal: Splice operation (not exposed publicly)
// -----------------------------------------------------------------------------

namespace detail
{

struct SpliceOp : DispatchOp<SpliceOp>
{
    int fd_in;
    int64_t off_in;
    int fd_out;
    int64_t off_out;
    unsigned int len;
    unsigned int flags;

    SpliceOp(IoContext& ctx, const int in, const int64_t off_in, const int out, const int64_t off_out,
             const unsigned int length, const unsigned int fl)
        : DispatchOp(&ctx), fd_in(in), off_in(off_in), fd_out(out), off_out(off_out), len(length), flags(fl)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, SpliceOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_splice(sqe, op.fd_in, op.off_in, op.fd_out, op.off_out, op.len, op.flags);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, SpliceOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_splice(sqe_op, op.fd_in, op.off_in, op.fd_out, op.off_out, op.len, op.flags);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

inline SpliceOp AsyncSplice(IoContext& ctx, const int fd_in, const int64_t off_in, const int fd_out,
                            const int64_t off_out, const unsigned int len, const unsigned int flags = 0)
{
    return SpliceOp(ctx, fd_in, off_in, fd_out, off_out, len, flags);
}

}  // namespace detail

// -----------------------------------------------------------------------------
// Exact Read/Write/Recv/Send Helpers
// -----------------------------------------------------------------------------

/// @brief Reads exactly buffer.size() bytes, looping on partial reads.
/// @param ctx The IoContext to run on
/// @param f File descriptor to read from
/// @param buffer Buffer to read into. MUST remain valid until operation completes.
/// @param offset Offset to read from (default: 0). kUseFilePos is not supported.
/// @return Task yielding Result<void> - success means all bytes read
///
/// @warning The buffer must remain valid until co_await returns!
/// @warning Returns error if EOF is reached before the buffer is filled.
///
/// @code
///   std::array<std::byte, 1024> buffer;
///   auto result = co_await AsyncReadExact(ctx, fd, buffer);
///   if (!result) {
///       // Handle error (including unexpected EOF)
///   }
/// @endcode
template <FileDescriptor F>
Task<Result<void>> AsyncReadExact(IoContext& ctx, const F& f, std::span<std::byte> buffer, const uint64_t offset = 0)
{
    assert(offset != kUseFilePos && "AsyncReadExact does not support kUseFilePos");
    [[maybe_unused]] auto fd_owner = MaybeHoldFd(f);
    const int raw_fd = GetRawFd(f);

    size_t total = 0;
    const size_t target = buffer.size();

    while (total < target)
    {
        auto result = co_await AsyncRead(ctx, raw_fd, buffer.subspan(total), offset + total);
        if (!result)
        {
            co_return std::unexpected(result.error());
        }

        if (*result == 0)
        {
            co_return ErrorFromErrc(std::errc::no_message_available);  // EOF
        }

        total += *result;
    }

    co_return Result<void>{};
}

/// @brief Writes exactly buffer.size() bytes, looping on partial writes.
/// @param ctx The IoContext to run on
/// @param f File descriptor to write to
/// @param buffer Buffer to write from. MUST remain valid until operation completes.
/// @param offset Offset to write at (default: 0). kUseFilePos is not supported.
/// @return Task yielding Result<void> - success means all bytes written
///
/// @warning The buffer must remain valid until co_await returns!
///
/// @code
///   std::span<const std::byte> data = get_data();
///   co_await AsyncWriteExact(ctx, fd, data, file_offset);
/// @endcode
template <FileDescriptor F>
Task<Result<void>> AsyncWriteExact(IoContext& ctx, const F& f, std::span<const std::byte> buffer, uint64_t offset = 0)
{
    assert(offset != kUseFilePos && "AsyncWriteExact does not support kUseFilePos");
    [[maybe_unused]] auto fd_owner = MaybeHoldFd(f);
    const int raw_fd = GetRawFd(f);

    size_t total = 0;
    const size_t target = buffer.size();

    while (total < target)
    {
        auto result = co_await AsyncWrite(ctx, raw_fd, buffer.subspan(total), offset + total);
        if (!result)
        {
            co_return std::unexpected(result.error());
        }

        if (*result == 0)
        {
            co_return ErrorFromErrc(std::errc::no_message_available);
        }

        total += *result;
    }

    co_return Result<void>{};
}

/// @brief Receives exactly buffer.size() bytes, looping on partial receives.
/// @param ctx The IoContext to run on
/// @param f Socket to receive from
/// @param buffer Buffer to receive into. MUST remain valid until operation completes.
/// @param flags Optional recv flags (default: 0)
/// @return Task yielding Result<void> - success means all bytes received
///
/// @warning The buffer and socket must remain valid until co_await returns!
/// @warning Returns error if connection closes before buffer is filled.
///
/// @code
///   // Read a fixed-size header
///   Header header;
///   auto buf = std::as_writable_bytes(std::span(&header, 1));
///   auto result = co_await AsyncRecvExact(ctx, socket, buf);
///   if (!result) {
///       // Connection closed or error
///   }
/// @endcode
template <FileDescriptor F>
Task<Result<void>> AsyncRecvExact(IoContext& ctx, const F& f, std::span<std::byte> buffer, int flags = 0)
{
    size_t total = 0;
    const size_t target = buffer.size();

    while (total < target)
    {
        auto result = co_await AsyncRecv(ctx, f, buffer.subspan(total), flags);
        if (!result)
        {
            co_return std::unexpected(result.error());
        }

        if (*result == 0)
        {
            co_return ErrorFromErrc(std::errc::no_message_available);  // Connection closed
        }

        total += *result;
    }

    co_return Result<void>{};
}

/// @brief Sends exactly buffer.size() bytes, looping on partial sends.
/// @param ctx The IoContext to run on
/// @param f Socket to send to
/// @param buffer Buffer to send from. MUST remain valid until operation completes.
/// @param flags Optional send flags (default: 0)
/// @return Task yielding Result<void> - success means all bytes sent
///
/// @warning The buffer and socket must remain valid until co_await returns!
///
/// @code
///   std::string_view response = "HTTP/1.1 200 OK\r\n\r\n";
///   auto data = std::as_bytes(std::span(response));
///   co_await AsyncSendExact(ctx, client_socket, data);
/// @endcode
template <FileDescriptor F>
Task<Result<void>> AsyncSendExact(IoContext& ctx, const F& f, std::span<const std::byte> buffer, int flags = 0)
{
    size_t total = 0;
    const size_t target = buffer.size();

    while (total < target)
    {
        auto result = co_await AsyncSend(ctx, f, buffer.subspan(total), flags);
        if (!result)
        {
            co_return std::unexpected(result.error());
        }

        if (*result == 0)
        {
            co_return ErrorFromErrc(std::errc::no_message_available);
        }

        total += *result;
    }

    co_return Result<void>{};
}

// -----------------------------------------------------------------------------
// Sendfile Helper
// -----------------------------------------------------------------------------

/// @brief Zero-copy file-to-socket transfer using splice via a pooled pipe.
/// @param ctx The IoContext to run on
/// @param out_fd Destination socket. MUST remain valid until operation completes.
/// @param in_fd Source file descriptor. MUST remain valid until operation completes.
/// @param offset Starting offset in the source file
/// @param count Number of bytes to transfer
/// @return Task yielding Result<void> - success means all bytes transferred
///
/// @details Transfers 'count' bytes from 'in_fd' starting at 'offset' to 'out_fd'.
///          Uses the IoContext's pipe pool for efficiency across multiple calls,
///          avoiding pipe creation overhead on each invocation.
///
/// @warning Both file descriptors must remain open until co_await returns!
/// @warning Returns error if EOF on input file or output socket closes early.
///
/// @code
///   // Serve a static file over a socket
///   int file_fd = open("index.html", O_RDONLY);
///   struct stat st;
///   fstat(file_fd, &st);
///
///   auto result = co_await AsyncSendfile(ctx, client_socket, file_fd, 0, st.st_size);
///   close(file_fd);
///
///   if (!result) {
///       // Transfer failed (client disconnected, etc.)
///   }
/// @endcode
template <FileDescriptor Fout, FileDescriptor Fin>
Task<Result<void>> AsyncSendfile(IoContext& ctx, const Fout& out_fd, const Fin& in_fd, off_t offset, size_t count)
{
    [[maybe_unused]] auto out_fd_owner = MaybeHoldFd(out_fd);
    [[maybe_unused]] auto in_fd_owner = MaybeHoldFd(in_fd);
    const int raw_out_fd = GetRawFd(out_fd);
    const int raw_in_fd = GetRawFd(in_fd);

    // Acquire a pipe from the pool
    auto pipe_guard = ctx.GetPipePool().AcquireGuarded();
    if (!pipe_guard)
    {
        co_return ErrorFromErrc(std::errc::too_many_files_open);
    }

    auto& pipe = pipe_guard->get();
    const int pipe_read = pipe.read_fd;
    const int pipe_write = pipe.write_fd;

    size_t remaining = count;
    off_t current_offset = offset;

    constexpr size_t kChunkSize = 65536;  // 64KB chunks

    while (remaining > 0)
    {
        const auto to_splice = static_cast<unsigned int>(std::min(remaining, kChunkSize));

        // Splice from File -> Pipe
        // Loop until we get 'to_splice' bytes or EOF
        size_t bytes_in_pipe = 0;
        while (bytes_in_pipe < to_splice)
        {
            auto res = co_await detail::AsyncSplice(ctx, raw_in_fd, current_offset, pipe_write, -1,
                                                    to_splice - bytes_in_pipe, 0);
            if (!res)
                co_return std::unexpected(res.error());
            if (*res == 0)
                break;  // EOF
            bytes_in_pipe += *res;
            current_offset += *res;
        }

        if (bytes_in_pipe == 0)
            break;  // Real EOF

        // Splice from Pipe -> Socket
        // Must flush EXACTLY bytes_in_pipe
        size_t bytes_flushed = 0;
        while (bytes_flushed < bytes_in_pipe)
        {
            auto res = co_await detail::AsyncSplice(ctx, pipe_read, -1, raw_out_fd, -1,
                                                    bytes_in_pipe - bytes_flushed, 0);
            if (!res)
                co_return std::unexpected(res.error());
            if (*res == 0)
                co_return ErrorFromErrc(std::errc::broken_pipe);

            bytes_flushed += *res;
        }

        remaining -= bytes_in_pipe;
    }

    co_return Result<void>{};
}

}  // namespace kio
