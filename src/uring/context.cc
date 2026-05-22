#include "uring/context.h"

#include <cassert>
#include <cstring>
#include <future>

#include <unistd.h>

#include <sys/eventfd.h>

namespace URing
{

void IoWorker::init(InternalKey, const int wq_fd)
{
    io_uring_params params{};

    params.flags |= opts_.flags;

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

    // set the current io
    tl_io = this;

    ALOG_INFO("Started IO context with {} entries (SQPOLL: {})", opts_.entries,
              (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
}

void IoWorker::submit_or_wait_for()
{
    // Only block if we have no local work to do.

    if (io_uring_cq_ready(&ring_) > 0 || !queue_.empty())
    {
        if (const auto ret = io_uring_submit(&ring_); ret < 0 && ret != -EINTR)
        {
            ALOG_ERROR("failed to submit: {}", std::strerror(-ret));
        }
    }
    else
    {
        if (const auto ret = io_uring_submit_and_wait(&ring_, 1); ret < 0 && ret != -EINTR)
        {
            ALOG_ERROR("failed to submit and wait: {}", std::strerror(-ret));
        }
    }
}
void IoWorker::tick(const std::size_t batch_max_size) noexcept
{
    // Step 1: Drain the ready queue first.
    queue_.drain(
        [](const std::coroutine_handle<> h)
        {
            // Safe by invariant; transfers to next await point
            h.resume();
        },
        batch_max_size);

    // Step 2: wait for completion if needed
    submit_or_wait_for();

    // 3. Batch process CQEs
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        count++;
        handle_cqe(cqe);
    }

    if (count > 0)
    {
        io_uring_cq_advance(&ring_, count);
    }
}

void IoWorker::handle_cqe(io_uring_cqe* cqe)
{
    auto user_data = io_uring_cqe_get_data64(cqe);

    if (user_data == kWakeupSentinel)
    {
        arm_wake_read();
        return;
    }

    // Reconstruct coroutine handle from user_data.
    // Safety: user_data was set from h.address() while suspended.
    // The coroutine frame is guaranteed to outlive the pending I/O
    // (lifetime contract). from_address() is standard-compliant.
    auto h = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(user_data));

    // Enqueue rather than direct resume to maintain ordering and
    // ensure execution happens on the correct thread context.
    queue_.enqueue(h);
}

void IoWorker::arm_wake_read() noexcept
{
    io_uring_sqe* sqe = get_sqe(key_);
    io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
    io_uring_sqe_set_data64(sqe, kWakeupSentinel);
    io_uring_submit(&ring_);
}

void IoWorker::run(InternalKey key, std::size_t batch_max_size, std::stop_token st) noexcept
{
    // owner thread should be set on the thread which start the loop
    owner_thread_ = std::this_thread::get_id();

    std::stop_callback wake_on_stop{st, [this, key] { wake(key); }};
    while (!st.stop_requested())
    {
        tick(batch_max_size);
    }

    ALOG_INFO("Worker {} quiescing...", id_);
    // TODO: implement cleanup here

    // reset the tls context
    tl_io = nullptr;
}

io_uring_sqe* IoWorker::get_sqe(InternalKey) noexcept
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

void IoWorker::wake(InternalKey) const noexcept
{
    constexpr uint64_t one = 1;
    for (;;)
    {
        const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
        if (n == sizeof(one))
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

IoWorker::~IoWorker()
{
    // TODO: cancell all ops on the ring fd

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

//
// OP Context
//
IoContext::IoContext(const std::size_t num_threads, const IoOptions& opts)
    : num_threads_(num_threads), opts_(opts), start_latch_(num_threads)
{
    contexts_.reserve(num_threads);
    workers_.reserve(num_threads);

    if (num_threads == 0)
    {
        throw std::runtime_error("io context started with 0 thread");
    }
}

IoContext::~IoContext()
{
    if (running_.load(std::memory_order_acquire))
    {
        (void)stop();
    }
    // Explicitly join workers before deleting contexts_
    workers_.clear();
    contexts_.clear();
}
}  // namespace URing
