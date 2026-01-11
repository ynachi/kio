//
// Created by Yao ACHI on 11/01/2026.
//

#ifndef KIO_IO_AWAITERS_H
#define KIO_IO_AWAITERS_H
#include "io_uring_executor.h"
namespace kio::io::next
{
inline auto read(kio::next::IoUringExecutor* executor, int fd, void* buf, size_t len, off_t offset = 0)
{
    return make_io_awaiter<ssize_t>(executor,
                                    [=](io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, buf, len, offset); });
}

inline auto write(kio::next::IoUringExecutor* executor, int fd, const void* buf, size_t len, off_t offset = 0)
{
    return make_io_awaiter<ssize_t>(executor,
                                    [=](io_uring_sqe* sqe) { io_uring_prep_write(sqe, fd, buf, len, offset); });
}

inline auto fsync(kio::next::IoUringExecutor* executor, int fd, int flags = 0)
{
    return make_io_awaiter<int>(executor, [=](io_uring_sqe* sqe) { io_uring_prep_fsync(sqe, fd, flags); });
}

/**
 * @brief Accept a connection
 */
inline auto accept(kio::next::IoUringExecutor* exec, int listen_fd, sockaddr* addr, socklen_t* addrlen)
{
    return make_io_awaiter<int>(exec,
                                [=](io_uring_sqe* sqe) { io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0); });
}

/**
 * @brief Receive data
 */
inline auto recv(kio::next::IoUringExecutor* exec, int fd, void* buf, size_t len, int flags = 0)
{
    return make_io_awaiter<ssize_t>(exec, [=](io_uring_sqe* sqe) { io_uring_prep_recv(sqe, fd, buf, len, flags); });
}

/**
 * @brief Send data
 */
inline auto send(kio::next::IoUringExecutor* exec, int fd, const void* buf, size_t len, int flags = 0)
{
    return make_io_awaiter<ssize_t>(exec, [=](io_uring_sqe* sqe) { io_uring_prep_send(sqe, fd, buf, len, flags); });
}

/**
 * @brief Connect to a remote address
 */
inline auto connect(kio::next::IoUringExecutor* exec, int fd, const sockaddr* addr, socklen_t addrlen)
{
    return make_io_awaiter<int>(exec, [=](io_uring_sqe* sqe) { io_uring_prep_connect(sqe, fd, addr, addrlen); });
}
}  // namespace kio::io::next
#endif  // KIO_IO_AWAITERS_H
