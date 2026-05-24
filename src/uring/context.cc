#include "uring/context.h"

#include <cassert>
#include <cstring>
#include <future>

#include <unistd.h>

#include <sys/eventfd.h>

#include "uring/awaiter.hpp"

namespace URing
{

void IO::init(const int wq_fd)
{
    io_uring_params params{};

    // disable the ring first
    params.flags = opts_.flags | IORING_SETUP_R_DISABLED;

    // attach WQ
    if (wq_fd >= 0)
    {
        params.flags |= IORING_SETUP_ATTACH_WQ;
        params.wq_fd = wq_fd;
    }

    // Safely configure SQPOLL if requested
    if (params.flags & IORING_SETUP_SQPOLL)
    {
        params.sq_thread_idle = opts_.sq_thread_idle_ms;
        if (opts_.sq_thread_cpu >= 0)
        {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = opts_.sq_thread_cpu;
        }
    }

    if (const int rc = io_uring_queue_init_params(opts_.entries, &ring_, &params); rc < 0)
    {
        throw std::runtime_error(std::format("io_uring_queue_init_params failed: {}", std::strerror(-rc)));
    }

    wake_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wake_fd_ < 0)
    {
        io_uring_queue_exit(&ring_);
        throw std::runtime_error("eventfd failed");
    }
    arm_wake_read();

    ALOG_INFO("Started IO context with {} entries (SQPOLL: {})", opts_.entries,
              (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
}

void IO::submit_or_wait_for()
{
    // Only block if we have no local work to do.

    if (io_uring_cq_ready(&ring_) > 0 || !local_tasks_.empty() || !queue_.empty())
    {
        if (const auto ret = io_uring_submit(&ring_); ret < 0 && ret != -EINTR)
        {
            ALOG_ERROR("failed to submit: {}", std::strerror(-ret));
        }
    }
    else
    {
        // Thundering herd mitigation from our earlier optimizations
        is_sleeping_.store(true, std::memory_order_seq_cst);

        // Double check all three sources of work before sleeping
        if (io_uring_cq_ready(&ring_) == 0 && local_tasks_.empty() && queue_.empty())
        {
            if (const auto ret = io_uring_submit_and_wait(&ring_, 1); ret < 0 && ret != -EINTR)
            {
                ALOG_ERROR("failed to submit and wait: {}", std::strerror(-ret));
            }
        }

        is_sleeping_.store(false, std::memory_order_relaxed);
    }
}

void IO::tick(const std::size_t batch_max_size) noexcept
{
    // Drain the cross-thread MPSC queue first.
    queue_.drain(
        [this](task_promise_base* node)
        {
            // We retrieve the safe handle to resume later.
            local_tasks_.push_back(node->self_handle);
        },
        batch_max_size);

    // wait for completion if needed
    submit_or_wait_for();

    // Batch process CQEs into the local queue
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        count++;
        auto user_data = io_uring_cqe_get_data64(cqe);

        if (user_data == kWakeupSentinel)
        {
            arm_wake_read();
        }
        else
        {
            auto* op = reinterpret_cast<IoOps*>(user_data);
            op->res = cqe->res;

            local_tasks_.push_back(op->h);
        }
    }

    if (count > 0)
    {
        // Free all the kernel slots at once
        io_uring_cq_advance(&ring_, count);
    }

    // Execute all I/O completions immediately in this tick
    if (!local_tasks_.empty())
    {
        current_batch.swap(local_tasks_);

        for (auto h : current_batch)
        {
            h.resume();
        }
        current_batch.clear();
    }
}

void IO::arm_wake_read() noexcept
{
    io_uring_sqe* sqe = get_sqe();
    io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
    io_uring_sqe_set_data64(sqe, kWakeupSentinel);
    io_uring_submit(&ring_);
}

void IO::run(const std::size_t batch_max_size, std::stop_token st) noexcept
{
    std::stop_callback wake_on_stop{st, [this] { wake(); }};
    while (!st.stop_requested())
    {
        tick(batch_max_size);
    }

    ALOG_INFO("Worker {} quiescing...", id_);
    // TODO: implement cleanup here
}

io_uring_sqe* IO::get_sqe() noexcept
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr)
    {
        // SQ is full. Try one submit to clear space.
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
}

void IO::wake() const noexcept
{
    // Only write to the eventfd if the thread is actually parked in the kernel
    if (!is_sleeping_.load(std::memory_order_seq_cst))
    {
        return;
    }

    constexpr uint64_t one = 1;
    for (;;)
    {
        const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
        if (n == sizeof(one) || (n == -1 && errno == EAGAIN))
        {
            return;
        }
        if (n == -1 && errno == EINTR)
        {
            continue;
        }
        ALOG_WARN("failed to wake io context with eventfd: {}", std::strerror(errno));
        return;
    }
}

IO::~IO()
{
    // TODO: cancel all ops on the ring fd
    join();

    if (ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&ring_);
        ring_.ring_fd = -1;
    }

    if (wake_fd_ >= 0)
    {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}
}  // namespace URing
