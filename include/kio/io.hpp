#pragma once

#include "kio/net.hpp"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>

#include <fcntl.h>

#include "core/core.hpp"

namespace kio
{
//================================================================
// IO Buffer
//==============================================================

/**
 * @brief Linear I/O buffer with a three-region layout and two-phase writes.
 *
 * This is NOT a ring buffer. It is a flat, heap-allocated buffer with three
 * monotonically-increasing position indices:
 *
 *   read_ <= commit_ <= write_ <= capacity_
 *
 * Regions:
 *   [read_,   commit_) = Published (readable) bytes.
 *   [commit_, write_)  = Staged bytes (written but not yet published).
 *   [write_,  capacity_) = Free writable space.
 *
 * When the read region becomes large enough, Compact() slides live data to the
 * front of the buffer to reclaim space without reallocation.
 *
 * Thread safety: NOT thread-safe. Use one buffer per thread.
 *
 * Move-only: the internal allocation is managed by std::unique_ptr.
 *
 * Direct I/O pattern (e.g., recv/send):
 * @code
 *   buf.EnsureWritableBytes(4096);
 *   ssize_t n = recv(fd, buf.WritableSpan().data(), buf.WritableSpan().size(), 0);
 *   buf.Commit(n);           // publish n bytes as readable
 *   auto data = buf.ReadableSpan();
 *   process(data);
 *   buf.Consume(data.size());
 * @endcode
 *
 * Two-phase output building pattern (e.g., protocol responses):
 * @code
 *   buf.Append("HTTP/1.1 200 OK\r\n");
 *   buf.Append(header_line);
 *   buf.Append("\r\n");
 *   buf.Commit();          // publish all staged bytes atomically
 *   // or buf.RollbackPending() to discard
 * @endcode
 */
class IoBuffer
{
public:
    explicit IoBuffer(const std::size_t initial_capacity = 0) { Reserve(initial_capacity); }

    // ---------------------------
    // Core state
    // ---------------------------
    [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

    // Published (readable) bytes: [read_, commit_)
    [[nodiscard]] std::size_t ReadableBytes() const noexcept { return commit_ - read_; }

    // Staged bytes (built but not yet published): [commit_, write_)
    [[nodiscard]] std::size_t StagedBytes() const noexcept { return write_ - commit_; }

    // Free writable bytes: [write_, capacity_)
    [[nodiscard]] std::size_t WritableBytes() const noexcept { return capacity_ - write_; }

    [[nodiscard]] bool Empty() const noexcept { return ReadableBytes() == 0; }

    // ---------------------------
    // Readable view (CONTIGUOUS)
    // ---------------------------
    [[nodiscard]] std::span<const std::byte> ReadableSpan() const noexcept
    {
        if (!data_ || ReadableBytes() == 0)
            return {};
        return {data_.get() + read_, ReadableBytes()};
    }

    [[nodiscard]] std::span<const char> ReadableStr() const noexcept
    {
        const auto b = ReadableSpan();
        return {reinterpret_cast<const char*>(b.data()), b.size()};
    }

    // No allocations: single iovec.
    // const_cast is required: POSIX iov_base is void* (not const void*), so
    // callers must not write through this iovec for read-only data.
    [[nodiscard]] iovec ReadableIovec() const noexcept
    {
        auto b = ReadableSpan();
        return iovec{const_cast<std::byte*>(b.data()), b.size()};
    }

    // ---------------------------
    // Writable view
    // ---------------------------
    void EnsureWritableBytes(const std::size_t additional)
    {
        if (WritableBytes() >= additional)
            return;

        // Try compaction first if it will help.
        const std::size_t live = write_ - read_;
        const std::size_t free_if_compact = capacity_ - live;

        if (free_if_compact >= additional && read_ >= kCompactThreshold)
        {
            Compact();
            if (WritableBytes() >= additional)
                return;
        }

        // Grow if still not enough.
        Grow(live + additional);
    }

    [[nodiscard]] std::span<std::byte> WritableSpan() noexcept
    {
        if (!data_)
            return {};
        return {data_.get() + write_, WritableBytes()};
    }

    [[nodiscard]] iovec WritableIovec() noexcept
    {
        auto b = WritableSpan();
        return iovec{b.data(), b.size()};
    }

    // ---------------------------
    // Commit / rollback / consume
    // ---------------------------

    // Commit bytes written into WritableBytesSpan(): advances write_ and commit_.
    std::size_t Commit(std::size_t n) noexcept
    {
        const std::size_t actual = std::min(n, WritableBytes());
        write_ += actual;
        commit_ += actual;
        return actual;
    }

    // Publish staged bytes (for output building).
    void Commit() noexcept { commit_ = write_; }

    // Drop staged bytes.
    void RollbackPending() noexcept { write_ = commit_; }

    // Consume published bytes after processing/sending.
    std::size_t Consume(std::size_t n) noexcept
    {
        const std::size_t actual = std::min(n, ReadableBytes());
        read_ += actual;

        // Reset indices if fully empty (keeps counters from growing forever).
        if (read_ == commit_ && commit_ == write_)
        {
            read_ = commit_ = write_ = 0;
        }
        return actual;
    }

    void Clear() noexcept { read_ = commit_ = write_ = 0; }

    // ---------------------------
    // Append helpers (staged)
    // ---------------------------
    // Stages bytes into the write region WITHOUT publishing them as readable.
    // Call Commit() (no-arg) to publish all staged bytes, or RollbackPending()
    // to discard them. This two-phase model lets you build a response atomically.
    void Append(std::span<const std::byte> data)
    {
        EnsureWritableBytes(data.size());
        std::memcpy(data_.get() + write_, data.data(), data.size());
        write_ += data.size();
    }

    void Append(std::string_view sv) { Append(std::span{reinterpret_cast<const std::byte*>(sv.data()), sv.size()}); }

    template <std::size_t N>
    void Append(const char (&lit)[N])
    {
        static_assert(N > 0);
        Append(std::string_view(lit, N - 1));
    }

    // Optional helper used by text protocols.
    [[nodiscard]] std::optional<std::size_t> FindCrlf() const noexcept
    {
        auto s = ReadableStr();
        std::string_view sv(s.data(), s.size());
        auto pos = sv.find("\r\n");
        if (pos == std::string_view::npos)
            return std::nullopt;
        return pos;
    }

    void Reserve(const std::size_t min_capacity)
    {
        if (min_capacity <= capacity_)
            return;

        const std::size_t live = write_ - read_;
        const std::size_t new_cap = std::max(kMinCapacity, RoundUpPow2(min_capacity));

        auto new_data = Alloc(new_cap);
        if (data_ && live != 0)
        {
            std::memcpy(new_data.get(), data_.get() + read_, live);
        }

        // Rebase indices
        write_ = live;
        commit_ = (commit_ - read_);
        read_ = 0;

        data_ = std::move(new_data);
        capacity_ = new_cap;
    }

private:
    static constexpr std::size_t kMinCapacity = 4096;
    static constexpr std::size_t kCompactThreshold = 1024;

    static std::size_t RoundUpPow2(std::size_t n)
    {
        if (n <= 1)
            return 1;
        return std::bit_ceil(n);
    }

    static std::unique_ptr<std::byte[]> Alloc(std::size_t n)
    {
#if defined(__cpp_lib_make_unique_for_overwrite) && (__cpp_lib_make_unique_for_overwrite >= 202002L)
        return std::make_unique_for_overwrite<std::byte[]>(n);
#else
        return std::unique_ptr<std::byte[]>(new std::byte[n]);
#endif
    }

    void Compact()
    {
        if (!data_ || read_ == 0)
            return;

        const std::size_t live = write_ - read_;
        const std::size_t published = commit_ - read_;

        if (live > 0)
        {
            std::memmove(data_.get(), data_.get() + read_, live);
        }

        read_ = 0;
        commit_ = published;
        write_ = live;
    }

    void Grow(const std::size_t min_needed)
    {
        // amortized growth
        const std::size_t target = std::max(min_needed, capacity_ ? (capacity_ + 1) : kMinCapacity);
        Reserve(target);
    }

    std::unique_ptr<std::byte[]> data_{};
    std::size_t capacity_ = 0;

    std::size_t read_ = 0;
    std::size_t commit_ = 0;
    std::size_t write_ = 0;
};

///////////////////////////////////////////////////////////////////////
// Backend-neutral file handle
///////////////////////////////////////////////////////////////////////
template <typename Backend>
struct BasicFileHandle
{
    using native_handle_type = Backend::NativeFileHandle;

    native_handle_type handle = static_cast<native_handle_type>(-1);

    BasicFileHandle() = default;
    explicit BasicFileHandle(native_handle_type h) : handle(h) {}

    ~BasicFileHandle()
    {
        if constexpr (std::same_as<native_handle_type, int>)
        {
            if (handle >= 0)
            {
                ::close(handle);
                handle = -1;
            }
        }
    }

    BasicFileHandle(BasicFileHandle&& other) noexcept : handle(other.handle)
    {
        other.handle = static_cast<native_handle_type>(-1);
    }

    BasicFileHandle& operator=(BasicFileHandle&& other) noexcept
    {
        if (this != &other)
        {
            if constexpr (std::same_as<native_handle_type, int>)
            {
                if (handle >= 0)
                {
                    ::close(handle);
                }
            }
            handle = other.handle;
            other.handle = static_cast<native_handle_type>(-1);
        }
        return *this;
    }

    BasicFileHandle(const BasicFileHandle&) = delete;
    BasicFileHandle& operator=(const BasicFileHandle&) = delete;

    [[nodiscard]] native_handle_type Get() const { return handle; }

    [[nodiscard]] native_handle_type Release() noexcept
    {
        const native_handle_type temp = handle;
        handle = static_cast<native_handle_type>(-1);
        return temp;
    }
};

using FD = BasicFileHandle<UringBackend>;
using MemoryFD = BasicFileHandle<MemoryBackend>;

// -----------------------------------------------------------------------------
// Concepts & Helpers
// -----------------------------------------------------------------------------

// Matches int, or any type with a .get() -> int method (like Socket)
template <typename T>
concept FileDescriptor = std::convertible_to<T, int> || requires(const T& t) {
    { t.Get() } -> std::convertible_to<int>;
};

// Helper to extract the raw fd
constexpr int GetRawFd(const FileDescriptor auto& fd)
{
    if constexpr (std::convertible_to<decltype(fd), int>)
    {
        return static_cast<int>(fd);
    }
    else
    {
        return fd.Get();
    }
}

template <typename Backend>
constexpr auto GetFileHandle(const BasicFileHandle<Backend>& file) -> typename Backend::NativeFileHandle
{
    return file.Get();
}

struct AcceptResult
{
    int fd{-1};
    net::SocketAddress addr;
};

struct AcceptOp : DispatchOp<AcceptOp>
{
    int fd;
    net::SocketAddress client_addr{};

    template <FileDescriptor F>
    AcceptOp(IoContext& ctx, const F& f) : DispatchOp(&ctx), fd(GetRawFd(f))
    {
    }

    Result<AcceptResult> await_resume()
    {
        if (res < 0)
        {
            return std::unexpected(make_error_code(res));
        }
        return AcceptResult{res, client_addr};
    }
};

inline void Submit(UringBackend& backend, IoContext&, AcceptOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_accept(sqe, op.fd, op.client_addr.GetMutable(), &op.client_addr.addrlen, 0);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, AcceptOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_accept(sqe_op, op.fd, op.client_addr.GetMutable(), &op.client_addr.addrlen, 0);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Accepts an incoming connection on a listening socket.
/// @param ctx The IoContext to run on
/// @param f Listening socket file descriptor
/// @return Awaitable yielding Result<int> with the new client socket fd
///
/// @note The accepted client address is stored in the AcceptOp and can be
///       retrieved after co_await if needed.
///
/// @code
///   while (running) {
///       auto result = co_await AsyncAccept(ctx, listen_sock);
///       if (result) {
///           int client_fd = *result;
///           // Handle new connection
///       }
///   }
/// @endcode
template <FileDescriptor F>
AcceptOp AsyncAccept(IoContext& ctx, const F& f)
{
    return AcceptOp(ctx, f);
}

struct RecvOp : DispatchOp<RecvOp>
{
    int fd;
    std::span<std::byte> buffer;
    int flags;

    template <FileDescriptor F>
    RecvOp(IoContext& ctx, const F& f, std::span<std::byte> buf, int flags)
        : DispatchOp(&ctx), fd(GetRawFd(f)), buffer(buf), flags(flags)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, RecvOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_recv(sqe, op.fd, op.buffer.data(), op.buffer.size(), op.flags);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, RecvOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_recv(sqe_op, op.fd, op.buffer.data(), op.buffer.size(), op.flags);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Receives data from a socket.
/// @param ctx The IoContext to run on
/// @param f Socket to receive from
/// @param buffer Buffer to receive into. MUST remain valid until operation completes.
/// @param flags Optional recv flags (default: 0)
/// @return Awaitable yielding Result<size_t> with bytes received (0 = connection closed)
///
/// @warning The buffer must remain valid until co_await returns!
/// @warning Returns 0 bytes on graceful connection close (not an error).
///
/// @code
///   std::array<std::byte, 1024> buffer;
///   auto result = co_await AsyncRecv(ctx, socket, buffer);
///   if (result && *result > 0) {
///       // Process received data
///   } else if (result && *result == 0) {
///       // Connection closed by peer
///   }
/// @endcode
template <FileDescriptor F>
RecvOp AsyncRecv(IoContext& ctx, const F& f, std::span<std::byte> buffer, int flags = 0)
{
    return RecvOp{ctx, f, buffer, flags};
}

/// @brief Receives data into a char array (convenience overload).
template <FileDescriptor F, size_t N>
RecvOp AsyncRecv(IoContext& ctx, const F& f, char (&buf)[N], int flags = 0)
{
    return RecvOp{
        ctx, f, std::span{reinterpret_cast<std::byte*>(buf), N},
          flags
    };
}

/// @brief Receives data into a std::array (convenience overload).
template <FileDescriptor F, size_t N>
RecvOp AsyncRecv(IoContext& ctx, const F& f, std::array<std::byte, N>& buf, int flags = 0)
{
    return RecvOp{ctx, f, std::span{buf}, flags};
}

struct SendOp : DispatchOp<SendOp>
{
    int fd;
    std::span<const std::byte> buffer;
    int flags;

    template <FileDescriptor F>
    SendOp(IoContext& ctx, const F& f, std::span<const std::byte> buf, int flags)
        : DispatchOp(&ctx), fd(GetRawFd(f)), buffer(buf), flags(flags)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, SendOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_send(sqe, op.fd, op.buffer.data(), op.buffer.size(), op.flags);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, SendOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_send(sqe_op, op.fd, op.buffer.data(), op.buffer.size(), op.flags);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Sends data to a socket.
/// @param ctx The IoContext to run on
/// @param f Socket to send to
/// @param buffer Buffer to send from. MUST remain valid until operation completes.
/// @param flags Optional send flags (default: 0)
/// @return Awaitable yielding Result<size_t> with bytes sent
///
/// @warning The buffer must remain valid until co_await returns!
/// @note May send fewer bytes than requested (partial send). Use AsyncSendExact for complete sends.
///
/// @code
///   auto data = std::as_bytes(std::span(my_data));
///   auto result = co_await AsyncSend(ctx, socket, data);
///   if (result) {
///       size_t sent = *result;  // May be < data.size()
///   }
/// @endcode
template <FileDescriptor F>
[[nodiscard]] SendOp AsyncSend(IoContext& ctx, const F& f, std::span<const std::byte> buffer, int flags = 0)
{
    return SendOp{ctx, f, buffer, flags};
}

/// @brief Sends a string_view to a socket (convenience overload).
/// @note Perfect for sending HTTP responses or text protocols.
template <FileDescriptor F>
[[nodiscard]] SendOp AsyncSend(IoContext& ctx, const F& f, std::string_view str, int flags = 0)
{
    return SendOp{
        ctx, f, std::span{reinterpret_cast<const std::byte*>(str.data()), str.size()},
          flags
    };
}

/// @brief Sends a char array to a socket (convenience overload).
/// @note Automatically excludes the null terminator.
template <FileDescriptor F, size_t N>
[[nodiscard]] SendOp AsyncSend(IoContext& ctx, const F& f, const char (&buf)[N], int flags = 0)
{
    return SendOp{
        ctx, f, std::span{reinterpret_cast<const std::byte*>(buf), N - 1}, // Skip null terminator
        flags
    };
}

struct UnlinkAtOp : DispatchOp<UnlinkAtOp>
{
    int dirfd;
    std::filesystem::path path;
    int flags;

    UnlinkAtOp(IoContext& ctx, int d, std::filesystem::path p, int f)
        : DispatchOp(&ctx), dirfd(d), path(std::move(p)), flags(f)
    {
    }

    Result<> await_resume()
    {
        if (res < 0)
            return std::unexpected(make_error_code(res));
        return {};
    }
};

inline void Submit(UringBackend& backend, IoContext&, UnlinkAtOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_unlinkat(sqe, op.dirfd, op.path.c_str(), op.flags);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, UnlinkAtOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_unlinkat(sqe_op, op.dirfd, op.path.c_str(), op.flags);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

[[nodiscard]] inline UnlinkAtOp AsyncUnlink(IoContext& ctx, int dirfd, const std::filesystem::path path, int flags)
{
    return UnlinkAtOp(ctx, dirfd, path, flags);
}

[[nodiscard]] inline UnlinkAtOp AsyncUnlink(IoContext& ctx, const std::filesystem::path path, int flags = 0)
{
    return UnlinkAtOp(ctx, AT_FDCWD, path, flags);
}

struct MkdirAtOp : DispatchOp<MkdirAtOp>
{
    int dirfd;
    std::filesystem::path path;
    mode_t mode;

    MkdirAtOp(IoContext& ctx, int d, std::filesystem::path p, mode_t m)
        : DispatchOp(&ctx), dirfd(d), path(std::move(p)), mode(m)
    {
    }

    Result<> await_resume()
    {
        if (res < 0)
            return std::unexpected(make_error_code(res));
        return {};
    }
};

inline void Submit(UringBackend& backend, IoContext&, MkdirAtOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_mkdirat(sqe, op.dirfd, op.path.c_str(), op.mode);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, MkdirAtOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_mkdirat(sqe_op, op.dirfd, op.path.c_str(), op.mode);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

[[nodiscard]] inline MkdirAtOp AsyncMkdir(IoContext& ctx, int dirfd, const std::filesystem::path path,
                                          mode_t mode = 0755)
{
    return MkdirAtOp(ctx, dirfd, path, mode);
}

[[nodiscard]] inline MkdirAtOp AsyncMkdir(IoContext& ctx, const std::filesystem::path path, mode_t mode = 0755)
{
    return MkdirAtOp(ctx, AT_FDCWD, path, mode);
}

struct RenameAtOp : DispatchOp<RenameAtOp>
{
    int old_dirfd;
    std::filesystem::path old_path;
    int new_dirfd;
    std::filesystem::path new_path;
    unsigned flags;

    RenameAtOp(IoContext* ctx, int old_dfd, std::filesystem::path old_p, int new_dfd, std::filesystem::path new_p,
               unsigned rename_flags)
        : DispatchOp(ctx),
          old_dirfd(old_dfd),
          old_path(std::move(old_p)),
          new_dirfd(new_dfd),
          new_path(std::move(new_p)),
          flags(rename_flags)
    {
    }

    Result<> await_resume()
    {
        if (res < 0)
        {
            return std::unexpected(make_error_code(res));
        }
        return {};
    }
};

inline void Submit(UringBackend& backend, IoContext&, RenameAtOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_renameat(sqe, op.old_dirfd, op.old_path.c_str(), op.new_dirfd, op.new_path.c_str(), op.flags);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, RenameAtOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_renameat(sqe_op, op.old_dirfd, op.old_path.c_str(), op.new_dirfd, op.new_path.c_str(), op.flags);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

[[nodiscard]] inline RenameAtOp AsyncRename(IoContext& ctx, int old_dirfd, const std::filesystem::path old_path,
                                            const int new_dirfd, const std::filesystem::path new_path,
                                            const unsigned flags = 0)
{
    return RenameAtOp(&ctx, old_dirfd, old_path, new_dirfd, new_path, flags);
}

[[nodiscard]] inline RenameAtOp AsyncRename(IoContext& ctx, const std::filesystem::path old_path,
                                            const std::filesystem::path new_path, unsigned flags = 0)
{
    return RenameAtOp(&ctx, AT_FDCWD, old_path, AT_FDCWD, new_path, flags);
}

template <typename Backend = UringBackend>
struct OpenOp : DispatchOp<OpenOp<Backend>, Backend>
{
    std::filesystem::path path;
    int flags;
    mode_t mode;
    typename Backend::NativeFileHandle opened_handle{};

    OpenOp(BasicIoContext<Backend>& ctx, std::filesystem::path p, int f, mode_t m)
        : DispatchOp<OpenOp<Backend>, Backend>(&ctx), path(std::move(p)), flags(f), mode(m)
    {
    }

    Result<BasicFileHandle<Backend>> await_resume()
    {
        if (this->res < 0)
        {
            return std::unexpected(make_error_code(this->res));
        }
        if constexpr (std::same_as<Backend, MemoryBackend>)
        {
            return BasicFileHandle<Backend>(opened_handle);
        }
        else
        {
            return BasicFileHandle<Backend>(static_cast<Backend::NativeFileHandle>(this->res));
        }
    }
};

inline void Submit(UringBackend& backend, IoContext&, OpenOp<>& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_openat(sqe, AT_FDCWD, op.path.c_str(), op.flags, op.mode);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, OpenOp<>& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_openat(sqe_op, AT_FDCWD, op.path.c_str(), op.flags, op.mode);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

inline void Submit(MemoryBackend& backend, MemoryIoContext&, OpenOp<MemoryBackend>& op)
{
    const auto result = backend.OpenFile(op.path, op.flags, op.mode);
    if (!result)
    {
        backend.Complete(&op, -result.error().value());
        return;
    }

    op.opened_handle = *result;
    backend.Complete(&op, 0);
}

inline void SubmitWithTimeout(MemoryBackend& backend, MemoryIoContext& ctx, OpenOp<MemoryBackend>& op,
                              __kernel_timespec&)
{
    Submit(backend, ctx, op);
}

/// @brief Opens a file asynchronously.
/// @param ctx The IoContext to run on
/// @param path Path to the file
/// @param flags Open flags (O_RDONLY, O_CREAT, etc.)
/// @param mode File mode for creation (default 0644)
/// @return Awaitable yielding Result<int> with the new file descriptor
///
/// @code
///   auto fd_res = co_await AsyncOpen(ctx, "data.txt", O_RDONLY);
///   if (fd_res) {
///       int fd = *fd_res;
///       // Use fd...
///   }
/// @endcode
template <typename Backend>
[[nodiscard]] inline OpenOp<Backend> AsyncOpen(BasicIoContext<Backend>& ctx, const std::filesystem::path path,
                                               int flags, mode_t mode = 0644)
{
    return OpenOp<Backend>(ctx, path, flags, mode);
}

template <typename Backend = UringBackend>
struct ReadOp : DispatchOp<ReadOp<Backend>, Backend>
{
    typename Backend::NativeFileHandle fd;
    std::span<std::byte> buffer;
    uint64_t offset;

    ReadOp(BasicIoContext<Backend>& ctx, typename Backend::NativeFileHandle f, std::span<std::byte> buf, uint64_t off)
        : DispatchOp<ReadOp<Backend>, Backend>(&ctx), fd(f), buffer(buf), offset(off)
    {
    }

    ReadOp(BasicIoContext<Backend>& ctx, const BasicFileHandle<Backend>& f, std::span<std::byte> buf, uint64_t off)
        : DispatchOp<ReadOp<Backend>, Backend>(&ctx), fd(GetFileHandle(f)), buffer(buf), offset(off)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, ReadOp<UringBackend>& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_read(sqe, op.fd, op.buffer.data(), op.buffer.size(), op.offset);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, ReadOp<UringBackend>& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_read(sqe_op, op.fd, op.buffer.data(), op.buffer.size(), op.offset);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

inline void Submit(MemoryBackend& backend, MemoryIoContext&, ReadOp<MemoryBackend>& op)
{
    const auto result = backend.ReadFile(op.fd, op.buffer, op.offset);
    backend.Complete(&op, result ? static_cast<int32_t>(*result) : -result.error().value());
}

inline void SubmitWithTimeout(MemoryBackend& backend, MemoryIoContext& ctx, ReadOp<MemoryBackend>& op,
                              __kernel_timespec&)
{
    Submit(backend, ctx, op);
}

/// @brief Reads data from a file descriptor.
/// @param ctx The IoContext to run on
/// @param f File descriptor to read from
/// @param buffer Buffer to read into. MUST remain valid until operation completes.
/// @param offset File offset to read from (default: 0, use -1 for current position)
/// @return Awaitable yielding Result<size_t> with bytes read (0 = EOF)
///
/// @warning The buffer must remain valid until co_await returns!
///
/// @note EOF is not considered an error
///
/// @code
///   std::vector<std::byte> buffer(4096);
///   auto result = co_await AsyncRead(ctx, fd, buffer, file_offset);
///   if (result && *result > 0) {
///       // Process data
///   }
/// @endcode
template <typename Backend>
[[nodiscard]] ReadOp<Backend> AsyncRead(BasicIoContext<Backend>& ctx, const BasicFileHandle<Backend>& f,
                                        std::span<std::byte> buffer, uint64_t offset = 0)
{
    return ReadOp<Backend>{ctx, f, buffer, offset};
}

[[nodiscard]] inline ReadOp<> AsyncRead(IoContext& ctx, int fd, std::span<std::byte> buffer, uint64_t offset = 0)
{
    return ReadOp{ctx, fd, buffer, offset};
}

template <typename Backend = UringBackend>
struct WriteOp : DispatchOp<WriteOp<Backend>, Backend>
{
    Backend::NativeFileHandle fd;
    std::span<const std::byte> buffer;
    uint64_t offset;

    WriteOp(BasicIoContext<Backend>& ctx, Backend::NativeFileHandle f, std::span<const std::byte> buf, uint64_t off)
        : DispatchOp<WriteOp, Backend>(&ctx), fd(f), buffer(buf), offset(off)
    {
    }

    WriteOp(BasicIoContext<Backend>& ctx, const BasicFileHandle<Backend>& f, std::span<const std::byte> buf,
            uint64_t off)
        : DispatchOp<WriteOp, Backend>(&ctx), fd(GetFileHandle(f)), buffer(buf), offset(off)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, WriteOp<>& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_write(sqe, op.fd, op.buffer.data(), op.buffer.size(), op.offset);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, WriteOp<>& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_write(sqe_op, op.fd, op.buffer.data(), op.buffer.size(), op.offset);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

inline void Submit(MemoryBackend& backend, MemoryIoContext&, WriteOp<MemoryBackend>& op)
{
    const auto result = backend.WriteFile(op.fd, op.buffer, op.offset);
    backend.Complete(&op, result ? static_cast<int32_t>(*result) : -result.error().value());
}

inline void SubmitWithTimeout(MemoryBackend& backend, MemoryIoContext& ctx, WriteOp<MemoryBackend>& op,
                              __kernel_timespec&)
{
    Submit(backend, ctx, op);
}

/// @brief Writes data to a file descriptor.
/// @param ctx The IoContext to run on
/// @param f File descriptor to write to
/// @param buffer Buffer to write from. MUST remain valid until operation completes.
/// @param offset File offset to write at (default: 0, use -1 for current position)
/// @return Awaitable yielding Result<size_t> with bytes written
///
/// @warning The buffer must remain valid until co_await returns!
/// @note May write fewer bytes than requested. Use AsyncWriteExact for complete writes.
///
/// @code
///   auto data = std::as_bytes(std::span(record));
///   co_await AsyncWrite(ctx, fd, data, file_offset);
/// @endcode
template <typename Backend>
[[nodiscard]] WriteOp<Backend> AsyncWrite(BasicIoContext<Backend>& ctx, const BasicFileHandle<Backend>& f,
                                          std::span<const std::byte> buffer, uint64_t offset = 0)
{
    return WriteOp<Backend>{ctx, f, buffer, offset};
}

[[nodiscard]] inline WriteOp<> AsyncWrite(IoContext& ctx, int fd, std::span<const std::byte> buffer,
                                          uint64_t offset = 0)
{
    return WriteOp{ctx, fd, buffer, offset};
}

template <typename Backend = UringBackend>
struct CloseOp : DispatchOp<CloseOp<Backend>, Backend>
{
    typename Backend::NativeFileHandle fd;

    CloseOp(BasicIoContext<Backend>& ctx, Backend::NativeFileHandle f) : DispatchOp<CloseOp, Backend>(&ctx), fd(f) {}

    CloseOp(BasicIoContext<Backend>& ctx, const BasicFileHandle<Backend>& f)
        : DispatchOp<CloseOp, Backend>(&ctx), fd(GetFileHandle(f))
    {
    }

    Result<void> await_resume()
    {
        if (this->res < 0)
            return std::unexpected(make_error_code(this->res));
        return {};
    }
};

inline void Submit(UringBackend& backend, IoContext&, CloseOp<>& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_close(sqe, op.fd);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, CloseOp<>& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_close(sqe_op, op.fd);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

inline void Submit(MemoryBackend& backend, MemoryIoContext&, CloseOp<MemoryBackend>& op)
{
    const auto result = backend.CloseFile(op.fd);
    backend.Complete(&op, result ? 0 : -result.error().value());
}

inline void SubmitWithTimeout(MemoryBackend& backend, MemoryIoContext& ctx, CloseOp<MemoryBackend>& op,
                              __kernel_timespec&)
{
    Submit(backend, ctx, op);
}

struct ReadFixedOp : DispatchOp<ReadFixedOp>
{
    int file_index;
    void* buffer;
    size_t len;
    off_t offset;

    ReadFixedOp(IoContext& ctx, int idx, std::span<std::byte> buf, off_t off)
        : DispatchOp(&ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, ReadFixedOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_read(sqe, op.file_index, op.buffer, op.len, op.offset);
    sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, ReadFixedOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_read(sqe_op, op.file_index, op.buffer, op.len, op.offset);
    sqe_op->flags |= IOSQE_FIXED_FILE;
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Reads from a registered file using its index (IOSQE_FIXED_FILE).
/// @param ctx The IoContext to run on
/// @param idx Index into the registered file table (from IoContext::RegisterFiles)
/// @param buf Buffer to read into. MUST remain valid until operation completes.
/// @param off File offset to read from
/// @return Awaitable yielding Result<size_t> with bytes read
///
/// @warning The buffer must remain valid until co_await returns!
/// @note Requires files to be registered with IoContext::RegisterFiles() first.
///       Uses fixed file optimization for reduced kernel overhead.
///
/// @code
///   // Register files once at startup
///   std::array fds = {fd1, fd2, fd3};
///   ctx.RegisterFiles(fds);
///
///   // Use index instead of fd
///   co_await AsyncReadFixed(ctx, 0, buffer, offset);  // Reads from fd1
/// @endcode
[[nodiscard]] inline ReadFixedOp AsyncReadFixed(IoContext& ctx, int idx, std::span<std::byte> buf, off_t off)
{
    return ReadFixedOp(ctx, idx, buf, off);
}

struct WriteFixedOp : DispatchOp<WriteFixedOp>
{
    int file_index;
    const void* buffer;
    size_t len;
    off_t offset;

    WriteFixedOp(IoContext& ctx, int idx, std::span<const std::byte> buf, off_t off)
        : DispatchOp(&ctx), file_index(idx), buffer(buf.data()), len(buf.size()), offset(off)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, WriteFixedOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_write(sqe, op.file_index, op.buffer, op.len, op.offset);
    sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, WriteFixedOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_write(sqe_op, op.file_index, op.buffer, op.len, op.offset);
    sqe_op->flags |= IOSQE_FIXED_FILE;
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Writes to a registered file using its index (IOSQE_FIXED_FILE).
/// @param ctx The IoContext to run on
/// @param idx Index into the registered file table (from IoContext::RegisterFiles)
/// @param buf Buffer to write from. MUST remain valid until operation completes.
/// @param off File offset to write at
/// @return Awaitable yielding Result<size_t> with bytes written
///
/// @warning The buffer must remain valid until co_await returns!
/// @note Requires files to be registered with IoContext::RegisterFiles() first.
///       Uses fixed file optimization for reduced kernel overhead.
[[nodiscard]] inline WriteFixedOp AsyncWriteFixed(IoContext& ctx, int idx, std::span<const std::byte> buf, off_t off)
{
    return WriteFixedOp(ctx, idx, buf, off);
}

/// @brief Closes a file descriptor asynchronously.
/// @param ctx The IoContext to run on
/// @param s File descriptor to close
/// @return Awaitable yielding Result<void>
///
/// @note After co_await returns, the fd is closed and must not be used.
///
/// @code
///   co_await AsyncClose(ctx, client_socket);
///   // client_socket is now invalid
/// @endcode
[[nodiscard]] inline CloseOp<> AsyncClose(IoContext& ctx, net::Socket& s)
{
    const auto fd = s.Release();
    return CloseOp(ctx, fd);
}

inline CloseOp<> AsyncClose(IoContext& ctx, int fd)
{
    return CloseOp(ctx, fd);
}

inline CloseOp<> AsyncClose(IoContext& ctx, const FD& fd)
{
    return CloseOp(ctx, fd);
}

template <typename Backend>
[[nodiscard]] CloseOp<Backend> AsyncClose(BasicIoContext<Backend>& ctx, const BasicFileHandle<Backend>& fd)
{
    return CloseOp<Backend>(ctx, fd);
}

struct ConnectOp : DispatchOp<ConnectOp>
{
    int fd;
    sockaddr_storage addr_store{};
    socklen_t addrlen;

    template <FileDescriptor F>
    ConnectOp(IoContext& ctx, const F& f, const sockaddr* addr, socklen_t len)
        : DispatchOp(&ctx), fd(GetRawFd(f)), addrlen(len)
    {
        std::memcpy(&addr_store, addr, len);
    }

    Result<void> await_resume()
    {
        if (res < 0)
            return std::unexpected(make_error_code(res));
        return {};
    }
};

inline void Submit(UringBackend& backend, IoContext&, ConnectOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_connect(sqe, op.fd, reinterpret_cast<sockaddr*>(&op.addr_store), op.addrlen);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, ConnectOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_connect(sqe_op, op.fd, reinterpret_cast<sockaddr*>(&op.addr_store), op.addrlen);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Connects a socket to a remote address.
/// @param ctx The IoContext to run on
/// @param f Socket file descriptor (should be non-blocking)
/// @param addr Pointer to sockaddr with destination address
/// @param len Size of the sockaddr structure
/// @return Awaitable yielding Result<void>
///
/// @note The sockaddr is copied internally, so it does not need to remain valid.
///
/// @code
///   sockaddr_in server{};
///   server.sin_family = AF_INET;
///   server.sin_port = htons(8080);
///   inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);
///
///   auto result = co_await AsyncConnect(ctx, socket, (sockaddr*)&server, sizeof(server));
///   if (!result) {
///       // Connection failed
///   }
/// @endcode
template <FileDescriptor F>
ConnectOp AsyncConnect(IoContext& ctx, const F& f, const sockaddr* addr, socklen_t len)
{
    return ConnectOp(ctx, f, addr, len);
}

/// @brief Connects a socket to a remote address using SocketAddress.
/// @param ctx The IoContext to run on
/// @param f Socket file descriptor (should be non-blocking)
/// @param addr SocketAddress with destination address
/// @return Awaitable yielding Result<void>
///
/// @code
///   auto addr = kio::net::SocketAddress::V4(8080, "127.0.0.1");
///   auto result = co_await AsyncConnect(ctx, socket, addr);
/// @endcode
template <FileDescriptor F>
ConnectOp AsyncConnect(IoContext& ctx, const F& f, const net::SocketAddress& addr)
{
    return ConnectOp(ctx, f, addr.Get(), addr.addrlen);
}

template <typename Backend = UringBackend>
struct FsyncOp : DispatchOp<FsyncOp<Backend>, Backend>
{
    Backend::NativeFileHandle fd;

    FsyncOp(BasicIoContext<Backend>& ctx, Backend::NativeFileHandle f) : DispatchOp<FsyncOp, Backend>(&ctx), fd(f) {}

    FsyncOp(BasicIoContext<Backend>& ctx, const BasicFileHandle<Backend>& f)
        : DispatchOp<FsyncOp, Backend>(&ctx), fd(GetFileHandle(f))
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, FsyncOp<UringBackend>& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_fsync(sqe, op.fd, 0);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, FsyncOp<>& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_fsync(sqe_op, op.fd, 0);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

inline void Submit(MemoryBackend& backend, MemoryIoContext&, FsyncOp<MemoryBackend>& op)
{
    const auto result = backend.FsyncFile(op.fd);
    backend.Complete(&op, result ? 0 : -result.error().value());
}

inline void SubmitWithTimeout(MemoryBackend& backend, MemoryIoContext& ctx, FsyncOp<MemoryBackend>& op,
                              __kernel_timespec&)
{
    Submit(backend, ctx, op);
}

/// @brief Flushes file data and metadata to disk (fsync).
/// @param ctx The IoContext to run on
/// @param f File descriptor to sync
/// @return Awaitable yielding Result<size_t>
///
/// @note Ensures both data and metadata (size, timestamps, etc.) are persisted.
///       For data-only sync, use AsyncFdatasync which may be faster.
///
/// @code
///   co_await AsyncWrite(ctx, fd, data, offset);
///   co_await AsyncFsync(ctx, fd);  // Data is now durable
/// @endcode
template <typename Backend>
FsyncOp<Backend> AsyncFsync(BasicIoContext<Backend>& ctx, const BasicFileHandle<Backend>& f)
{
    return FsyncOp<Backend>(ctx, f);
}

inline FsyncOp<> AsyncFsync(IoContext& ctx, int fd)
{
    return FsyncOp(ctx, fd);
}

inline Task<Result<void>> AsyncFsyncDir(IoContext& ctx, const std::filesystem::path dir_path)
{
    auto dir_res = co_await AsyncOpen(ctx, dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!dir_res)
    {
        co_return std::unexpected(dir_res.error());
    }

    auto dir_fd = dir_res->Release();

    auto sync_res = co_await AsyncFsync(ctx, dir_fd);
    if (!sync_res)
    {
        const auto close_res = co_await AsyncClose(ctx, dir_fd);
        if (!close_res)
        {
            ALOG_ERROR("AsyncClose failed after AsyncFsyncDir error: {}", close_res.error().message());
        }
        co_return std::unexpected(sync_res.error());
    }

    auto close_res = co_await AsyncClose(ctx, dir_fd);
    if (!close_res)
    {
        co_return std::unexpected(close_res.error());
    }

    co_return Result<void>{};
}

struct FdatasyncOp : DispatchOp<FdatasyncOp>
{
    int fd;

    template <FileDescriptor F>
    FdatasyncOp(IoContext& ctx, const F& f) : DispatchOp(&ctx), fd(GetRawFd(f))
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, FdatasyncOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_fsync(sqe, op.fd, IORING_FSYNC_DATASYNC);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, FdatasyncOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_fsync(sqe_op, op.fd, IORING_FSYNC_DATASYNC);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Flushes file data to disk, skipping metadata (fdatasync).
/// @param ctx The IoContext to run on
/// @param f File descriptor to sync
/// @return Awaitable yielding Result<size_t>
///
/// @note Faster than fsync when metadata changes (like atime) don't need persistence.
///       Use this for write-ahead logs where only data durability matters.
///
/// @code
///   co_await AsyncWrite(ctx, wal_fd, log_entry, offset);
///   co_await AsyncFdatasync(ctx, wal_fd);  // Data durable, metadata may not be
/// @endcode
template <FileDescriptor F>
FdatasyncOp AsyncFdatasync(IoContext& ctx, const F& f)
{
    return FdatasyncOp(ctx, f);
}

struct FallocateOp : DispatchOp<FallocateOp>
{
    int fd;
    int mode;
    off_t offset;
    off_t len;

    template <FileDescriptor F>
    FallocateOp(IoContext& ctx, const F& f, int mode, off_t offset, off_t len)
        : DispatchOp(&ctx), fd(GetRawFd(f)), mode(mode), offset(offset), len(len)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, FallocateOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_fallocate(sqe, op.fd, op.mode, op.offset, op.len);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, FallocateOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_fallocate(sqe_op, op.fd, op.mode, op.offset, op.len);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Pre-allocates or manipulates file space (fallocate).
/// @param ctx The IoContext to run on
/// @param f File descriptor
/// @param mode Allocation mode (0 for default, FALLOC_FL_KEEP_SIZE, FALLOC_FL_PUNCH_HOLE, etc.)
/// @param offset Starting offset for the operation
/// @param len Number of bytes to allocate/deallocate
/// @return Awaitable yielding Result<size_t>
///
/// @note Pre-allocation avoids fragmentation and ensures space is available.
///       Common modes: 0 (allocate), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE (deallocate).
///
/// @code
///   // Pre-allocate 1GB for a database file
///   co_await AsyncFallocate(ctx, db_fd, 0, 0, 1024 * 1024 * 1024);
/// @endcode
template <FileDescriptor F>
FallocateOp AsyncFallocate(IoContext& ctx, const F& f, int mode, off_t offset, off_t len)
{
    return FallocateOp(ctx, f, mode, offset, len);
}

struct FtruncateOp : DispatchOp<FtruncateOp>
{
    int fd;
    off_t len;

    template <FileDescriptor F>
    FtruncateOp(IoContext& ctx, const F& f, const off_t len) : DispatchOp(&ctx), fd(GetRawFd(f)), len(len)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, FtruncateOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_ftruncate(sqe, op.fd, op.len);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, FtruncateOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_ftruncate(sqe_op, op.fd, op.len);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Truncates or extends a file to the specified length.
/// @param ctx The IoContext to run on
/// @param f File descriptor
/// @param len New file size in bytes
/// @return Awaitable yielding Result<size_t>
///
/// @note If len < current size, data beyond len is discarded.
///       If len > current size, file is extended with zero bytes (or a hole).
///
/// @code
///   // Truncate log file after rotation
///   co_await AsyncFtruncate(ctx, log_fd, 0);
/// @endcode
template <FileDescriptor F>
FtruncateOp AsyncFtruncate(IoContext& ctx, const F& f, off_t len)
{
    return FtruncateOp(ctx, f, len);
}

struct PollOp : DispatchOp<PollOp>
{
    int fd;
    unsigned poll_mask;

    template <FileDescriptor F>
    PollOp(IoContext& ctx, const F& f, unsigned mask) : DispatchOp(&ctx), fd(GetRawFd(f)), poll_mask(mask)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, PollOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_poll_add(sqe, op.fd, op.poll_mask);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, PollOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_poll_add(sqe_op, op.fd, op.poll_mask);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Waits for events on a file descriptor (poll).
/// @param ctx The IoContext to run on
/// @param f File descriptor to poll
/// @param poll_mask Events to wait for (POLLIN, POLLOUT, POLLERR, etc.)
/// @return Awaitable yielding Result<size_t> with the triggered events mask
///
/// @note Useful for waiting on special fds (eventfd, timerfd, signalfd) or
///       checking socket readiness without performing I/O.
///
/// @code
///   // Wait for socket to become readable
///   auto result = co_await AsyncPoll(ctx, socket, POLLIN);
///   if (result && (*result & POLLIN)) {
///       // Socket has data ready
///   }
/// @endcode
template <FileDescriptor F>
PollOp AsyncPoll(IoContext& ctx, const F& f, unsigned poll_mask)
{
    return PollOp(ctx, f, poll_mask);
}

struct ReadvOp : DispatchOp<ReadvOp>
{
    int fd;
    std::span<const iovec> iovecs;
    uint64_t offset;

    template <FileDescriptor F>
    ReadvOp(IoContext& ctx, const F& f, std::span<const iovec> iov, uint64_t off)
        : DispatchOp(&ctx), fd(GetRawFd(f)), iovecs(iov), offset(off)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, ReadvOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_readv(sqe, op.fd, op.iovecs.data(), static_cast<unsigned>(op.iovecs.size()), op.offset);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, ReadvOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_readv(sqe_op, op.fd, op.iovecs.data(), static_cast<unsigned>(op.iovecs.size()), op.offset);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Reads data into multiple buffers (scatter read).
/// @param ctx The IoContext to run on
/// @param f File descriptor to read from
/// @param iovecs Span of iovec structures describing buffers. MUST remain valid until operation completes.
/// @param offset File offset to read from (default: 0)
/// @return Awaitable yielding Result<size_t> with total bytes read
///
/// @warning All iovec buffers and the span itself must remain valid until co_await returns!
///
/// @code
///   Header header;
///   std::array<std::byte, 1024> payload;
///   std::array<iovec, 2> vecs = {{
///       {&header, sizeof(header)},
///       {payload.data(), payload.size()}
///   }};
///   auto result = co_await AsyncReadv(ctx, fd, vecs, file_offset);
/// @endcode
template <FileDescriptor F>
ReadvOp AsyncReadv(IoContext& ctx, const F& f, std::span<const iovec> iovecs, uint64_t offset = 0)
{
    return ReadvOp(ctx, f, iovecs, offset);
}

struct WritevOp : DispatchOp<WritevOp>
{
    int fd;
    std::span<const iovec> iovecs;
    uint64_t offset;

    template <FileDescriptor F>
    WritevOp(IoContext& ctx, const F& f, std::span<const iovec> iov, uint64_t off)
        : DispatchOp(&ctx), fd(GetRawFd(f)), iovecs(iov), offset(off)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, WritevOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_writev(sqe, op.fd, op.iovecs.data(), static_cast<unsigned>(op.iovecs.size()), op.offset);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, WritevOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_writev(sqe_op, op.fd, op.iovecs.data(), static_cast<unsigned>(op.iovecs.size()), op.offset);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Writes data from multiple buffers (gather write).
/// @param ctx The IoContext to run on
/// @param f File descriptor to write to
/// @param iovecs Span of iovec structures describing buffers. MUST remain valid until operation completes.
/// @param offset File offset to write at (default: 0)
/// @return Awaitable yielding Result<size_t> with total bytes written
///
/// @warning All iovec buffers and the span itself must remain valid until co_await returns!
///
/// @code
///   Header header = make_header();
///   std::span<const std::byte> payload = get_payload();
///   std::array<iovec, 2> vecs = {{
///       {&header, sizeof(header)},
///       {payload.data(), payload.size()}
///   }};
///   co_await AsyncWritev(ctx, fd, vecs, file_offset);
/// @endcode
template <FileDescriptor F>
WritevOp AsyncWritev(IoContext& ctx, const F& f, std::span<const iovec> iovecs, uint64_t offset = 0)
{
    return WritevOp(ctx, f, iovecs, offset);
}

struct SendmsgOp : DispatchOp<SendmsgOp>
{
    int fd;
    const msghdr* msg;
    unsigned flags;

    template <FileDescriptor F>
    SendmsgOp(IoContext& ctx, const F& f, const msghdr* m, unsigned fl)
        : DispatchOp(&ctx), fd(GetRawFd(f)), msg(m), flags(fl)
    {
    }
};

inline void Submit(UringBackend& backend, IoContext&, SendmsgOp& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_sendmsg(sqe, op.fd, op.msg, op.flags);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, SendmsgOp& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_sendmsg(sqe_op, op.fd, op.msg, op.flags);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

/// @brief Sends a message with optional ancillary data (sendmsg).
/// @param ctx The IoContext to run on
/// @param f Socket to send to
/// @param msg Pointer to msghdr structure. MUST remain valid until operation completes.
/// @param flags Optional send flags (default: 0)
/// @return Awaitable yielding Result<size_t> with bytes sent
///
/// @warning The msghdr and all referenced buffers must remain valid until co_await returns!
///
/// @note Use for scatter-gather sends, sending to specific addresses (UDP),
///       or passing ancillary data (e.g., file descriptors via SCM_RIGHTS).
///
/// @code
///   msghdr msg{};
///   std::array<iovec, 2> iov = {{...}};
///   msg.msg_iov = iov.data();
///   msg.msg_iovlen = iov.size();
///   co_await AsyncSendmsg(ctx, socket, &msg);
/// @endcode
template <FileDescriptor F>
SendmsgOp AsyncSendmsg(IoContext& ctx, const F& f, const msghdr* msg, unsigned flags = 0)
{
    return SendmsgOp(ctx, f, msg, flags);
}

template <typename Backend = UringBackend>
struct SleepOp : DispatchOp<SleepOp<Backend>, Backend>
{
    using DispatchOp<SleepOp<Backend>, Backend>::await_resume;

    __kernel_timespec ts{};

    template <typename Rep, typename Period>
    SleepOp(BasicIoContext<Backend>& ctx, std::chrono::duration<Rep, Period> dur)
        : DispatchOp<SleepOp<Backend>, Backend>(&ctx)
    {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
        ts.tv_sec = ns / 1'000'000'000;
        ts.tv_nsec = ns % 1'000'000'000;
    }

    SleepOp(SleepOp&& other) noexcept : DispatchOp<SleepOp<Backend>, Backend>(std::move(other)), ts(other.ts) {}

    Result<void> await_resume()
    {
        if (this->res == -ETIME || this->res == 0)
            return {};
        if (this->res < 0)
            return std::unexpected(make_error_code(this->res));
        return {};
    }
};

inline void Submit(UringBackend& backend, IoContext&, SleepOp<UringBackend>& op)
{
    auto* sqe = PrepareSqe(backend);
    io_uring_prep_timeout(sqe, &op.ts, 0, 0);
    io_uring_sqe_set_data(sqe, &op);
}

inline void SubmitWithTimeout(UringBackend& backend, IoContext&, SleepOp<>& op, __kernel_timespec& ts)
{
    auto [sqe_op, sqe_timer] = PrepareLinkedTimeoutSqes(backend, ts);
    io_uring_prep_timeout(sqe_op, &op.ts, 0, 0);
    sqe_op->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_op, &op);
}

inline void Submit(MemoryBackend& backend, MemoryIoContext&, SleepOp<MemoryBackend>& op)
{
    using namespace std::chrono;
    const auto due = backend.Now() + seconds(op.ts.tv_sec) + nanoseconds(op.ts.tv_nsec);
    op.res = 0;
    backend.AddTimer(&op, due);
}

/// @brief Suspends the coroutine for the specified duration.
/// @param ctx The IoContext to run on
/// @param dur Duration to sleep (any std::chrono::duration type)
/// @return Awaitable yielding Result<void>
///
/// @note Uses io_uring timeout for efficient kernel-level sleep.
///       Does not block the thread - other coroutines continue running.
///
/// @code
///   using namespace std::chrono_literals;
///
///   // Retry with backoff
///   for (int i = 0; i < 3; ++i) {
///       auto result = co_await try_connect();
///       if (result) break;
///       co_await AsyncSleep(ctx, 100ms * (1 << i));  // 100ms, 200ms, 400ms
///   }
/// @endcode
template <typename Rep, typename Period>
auto AsyncSleep(IoContext& ctx, std::chrono::duration<Rep, Period> dur)
{
    return SleepOp<>(ctx, dur);
}

template <typename Rep, typename Period>
auto AsyncSleep(MemoryIoContext& ctx, std::chrono::duration<Rep, Period> dur)
{
    return SleepOp<MemoryBackend>(ctx, dur);
}
}  // namespace kio
