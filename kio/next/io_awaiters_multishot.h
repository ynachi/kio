//
// io_awaiters_multishot.h - Multishot accept for high-throughput servers
// Requires Linux 5.19+ for IORING_OP_MULTISHOT
//

#ifndef KIO_IO_AWAITERS_MULTISHOT_H
#define KIO_IO_AWAITERS_MULTISHOT_H

#include "io_uring_executor.h"
#include <functional>

namespace kio::io::v1
{

/**
 * @brief Multishot accept awaiter - accepts multiple connections with one SQE
 *
 * This is MUCH more efficient than regular accept for high connection rate servers.
 * Requires kernel 5.19+.
 *
 * Usage:
 *   auto handler = [](int fd, sockaddr_in addr) {
 *       // Handle new connection
 *   };
 *   co_await multishot_accept(executor, listen_fd, handler);
 */
template<typename Handler>
class MultishotAcceptAwaiter
{
public:
    using Callback = std::function<void(int, sockaddr_in)>;

    MultishotAcceptAwaiter(next::v1::IoUringExecutor* executor,
                          int listen_fd,
                          Handler&& handler,
                          size_t max_accepts = 0)  // 0 = infinite
        : executor_(executor)
        , listen_fd_(listen_fd)
        , handler_(std::forward<Handler>(handler))
        , max_accepts_(max_accepts)
    {
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        continuation_ = h;

        // Get io_uring context
        size_t ctx_id = executor_->currentThreadInExecutor()
            ? executor_->currentContextId()
            : 0;

        io_uring* ring = executor_->getRing(ctx_id);
        io_uring_sqe* sqe = io_uring_get_sqe(ring);

        if (!sqe)
        {
            result_ = -EBUSY;
            return false;
        }

        // Prepare multishot accept
        io_uring_prep_multishot_accept(sqe, listen_fd_,
                                       reinterpret_cast<sockaddr*>(&addr_),
                                       &addr_len_, 0);

        io_uring_sqe_set_data(sqe, this);
        return true;
    }

    int await_resume()
    {
        if (result_ < 0)
        {
            errno = -result_;
            throw std::system_error(errno, std::system_category(),
                                   "multishot accept failed");
        }
        return result_;
    }

    // Called by event loop for each accepted connection
    void complete(int res)
    {
        if (res < 0)
        {
            // Error or cancelled - resume coroutine
            result_ = res;
            if (continuation_)
            {
                continuation_.resume();
            }
            return;
        }

        // New connection accepted!
        int client_fd = res;

        // Invoke handler (this distributes work across threads)
        handler_(client_fd, addr_);

        // Check if we should stop
        accepts_++;
        if (max_accepts_ > 0 && accepts_ >= max_accepts_)
        {
            // Cancel multishot and resume coroutine
            result_ = 0;
            if (continuation_)
            {
                continuation_.resume();
            }
        }

        // Otherwise, continue accepting (multishot stays active)
    }

private:
    next::v1::IoUringExecutor* executor_;
    int listen_fd_;
    Handler handler_;
    size_t max_accepts_;
    size_t accepts_ = 0;

    sockaddr_in addr_{};
    socklen_t addr_len_ = sizeof(addr_);

    std::coroutine_handle<> continuation_;
    int result_ = 0;
};

template<typename Handler>
auto multishot_accept(next::v1::IoUringExecutor* executor,
                     int listen_fd,
                     Handler&& handler,
                     size_t max_accepts = 0)
{
    return MultishotAcceptAwaiter<std::decay_t<Handler>>(
        executor, listen_fd, std::forward<Handler>(handler), max_accepts);
}

}  // namespace kio::io::v1

#endif  // KIO_IO_AWAITERS_MULTISHOT_H