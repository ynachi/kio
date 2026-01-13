//
// io_awaiters.h - I/O operation helpers with explicit control
//

#ifndef KIO_IO_AWAITERS_H
#define KIO_IO_AWAITERS_H

#include "io_uring_executor.h"

namespace kio::io::next
{

// ============================================================================
// Simple API (backward compatible - uses defaults)
// ============================================================================

inline auto read(kio::next::v1::IoUringExecutor* executor, int fd, void* buf, size_t len, off_t offset = 0)
{
    return kio::next::v1::make_io_awaiter<ssize_t>(executor,
        [=](io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, buf, len, offset); });
}

inline auto write(kio::next::v1::IoUringExecutor* executor, int fd, const void* buf, size_t len, off_t offset = 0)
{
    return kio::next::v1::make_io_awaiter<ssize_t>(executor,
        [=](io_uring_sqe* sqe) { io_uring_prep_write(sqe, fd, buf, len, offset); });
}

inline auto fsync(kio::next::v1::IoUringExecutor* executor, int fd, int flags = 0)
{
    return kio::next::v1::make_io_awaiter<int>(executor,
        [=](io_uring_sqe* sqe) { io_uring_prep_fsync(sqe, fd, flags); });
}

inline auto accept(kio::next::v1::IoUringExecutor* exec, int listen_fd, sockaddr* addr, socklen_t* addrlen)
{
    return kio::next::v1::make_io_awaiter<int>(exec,
        [=](io_uring_sqe* sqe) { io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0); });
}

inline auto recv(kio::next::v1::IoUringExecutor* exec, int fd, void* buf, size_t len, int flags = 0)
{
    return kio::next::v1::make_io_awaiter<ssize_t>(exec,
        [=](io_uring_sqe* sqe) { io_uring_prep_recv(sqe, fd, buf, len, flags); });
}

inline auto send(kio::next::v1::IoUringExecutor* exec, int fd, const void* buf, size_t len, int flags = 0)
{
    return kio::next::v1::make_io_awaiter<ssize_t>(exec,
        [=](io_uring_sqe* sqe) { io_uring_prep_send(sqe, fd, buf, len, flags); });
}

inline auto connect(kio::next::v1::IoUringExecutor* exec, int fd, const sockaddr* addr, socklen_t addrlen)
{
    return kio::next::v1::make_io_awaiter<int>(exec,
        [=](io_uring_sqe* sqe) { io_uring_prep_connect(sqe, fd, addr, addrlen); });
}

// ============================================================================
// Usage Examples (demonstrating explicit control)
// ============================================================================

/*
Example 1: Simple usage (backward compatible)
──────────────────────────────────────────────
auto n = co_await recv(executor, fd, buf, len);
// - Submits on current or picked context (automatic)
// - Resumes inline on submit context (fast)


Example 2: Explicit submission context
───────────────────────────────────────
auto n = co_await recv(executor, fd, buf, len)
    .on_context(3);  // Submit on context 3
// - Submits on context 3 (explicit)
// - Resumes inline on context 3 (fast)


Example 3: Connection affinity (same lane for all I/O)
───────────────────────────────────────────────────────
struct Connection {
    int fd;
    size_t affinity_lane;  // Chosen at accept
};

Lazy<void> handleClient(IoUringExecutor* exec, Connection& conn) {
    while (true) {
        // All I/O on same lane = cache locality
        auto n = co_await recv(exec, conn.fd, buf, sizeof(buf))
            .on_context(conn.affinity_lane);

        if (n <= 0) break;

        co_await send(exec, conn.fd, response, len)
            .on_context(conn.affinity_lane);
    }
}


Example 4: Resume on original context (hop back)
─────────────────────────────────────────────────
Lazy<Data> fetchFromDisk(IoUringExecutor* exec, int fd) {
    auto network_ctx = exec->checkout();  // Remember where we came from
    size_t storage_lane = 2;

    // Submit on storage lane, resume back on network thread
    auto n = co_await read(exec, fd, buf, len, offset)
        .on_context(storage_lane)     // Disk I/O on dedicated lane
        .resume_on(network_ctx);       // Hop back to network thread

    co_return parse(buf, n);
}


Example 5: Database sharding
────────────────────────────
Lazy<Row> fetchRow(IoUringExecutor* exec, int db_fd, RowKey key) {
    // Hash key to determine shard
    size_t shard_lane = hash(key) % NUM_SHARDS;
    auto home = exec->checkout();

    // Read from shard's lane (eliminates lock contention)
    auto bytes = co_await read(exec, db_fd, buf, len, offset)
        .on_context(shard_lane)
        .resume_on(home);

    co_return parse_row(buf, bytes);
}


Example 6: Hybrid pattern
──────────────────────────
Lazy<void> serveFile(IoUringExecutor* exec, int client_fd, std::string path) {
    auto network_lane = exec->currentContextId();
    size_t storage_lane = pick_storage_lane(path);

    int file_fd = open(path.c_str(), O_RDONLY);
    off_t offset = 0;

    while (true) {
        // Read from disk on storage lane, resume on network
        auto n = co_await read(exec, file_fd, buf, BUF_SIZE, offset)
            .on_context(storage_lane)
            .resume_on(network_lane);

        if (n <= 0) break;

        // Send on network lane (we're already here after resume_on)
        co_await send(exec, client_fd, buf, n);

        offset += n;
    }

    close(file_fd);
}
*/

}  // namespace kio::io::next

#endif  // KIO_IO_AWAITERS_H