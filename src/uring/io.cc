#include "uring/core/io.h"

#include <cassert>
#include <cstring>
#include <future>

#include <unistd.h>

#include <sys/eventfd.h>

#include "uring/core/awaiter.hpp"

namespace URing
{

IO::IO(const size_t id, const IO* leader, const IoOptions& opts, const std::initializer_list<BucketConfig> pool_configs)
    : opts_(opts), id_(id), buffer_pool_(pool_configs)
{
    local_tasks_.reserve(opts_.entries);
    current_batch.reserve(kMaxResumesPerTick);

    // init
    int leader_fd = -1;
    if (leader != nullptr)
    {
        leader_fd = leader->ring_fd();
    }
    init(leader_fd);
}

IO::IO(IO&& other) noexcept
    : is_activated_(other.is_activated_),
      is_running_(other.is_running_),
      is_sleeping_(other.is_sleeping_.load(std::memory_order_relaxed)),
      opts_(other.opts_),
      id_(other.id_),
      buffer_pool_(std::move(other.buffer_pool_))
{
    // Safety check, we SHOULD not move a running IO.
    if (is_running_ || !other.queue_.empty() || !other.local_tasks_.empty())
    {
        ALOG_FATAL("FATAL: IO move failed. IO must be inert (not running and empty) to move.");
        std::terminate();
    }

    // Transfer Ring
    ring_ = other.ring_;
    std::memset(&other.ring_, 0, sizeof(io_uring));
    other.ring_.ring_fd = -1;

    // Transfer FDs
    wake_fd_ = std::exchange(other.wake_fd_, -1);

    // Transfer state
    wake_value_ = other.wake_value_;
}

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

    // register pool if not empty
    if (buffer_pool_.bucket_count() != 0)
    {
        if (auto res = register_buffers(buffer_pool_); !res.has_value())
        {
            ALOG_ERROR("failed to register buffers: {}", res.error().message());
        }
        ALOG_INFO("the buffer pool have been registered to io_uring");
    }

    ALOG_INFO("Initialized IO context with {} entries (SQPOLL: {})", opts_.entries,
              (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
}

void IO::activate()
{
    if (is_activated_)
    {
        ALOG_DEBUG("IO is already activated, this is a noop");
        return;
    }

    if (const int ret = io_uring_register(ring_.ring_fd, IORING_REGISTER_ENABLE_RINGS, nullptr, 0); ret < 0)
    {
        throw std::runtime_error(std::format("io_uring_register failed: {}", std::strerror(-ret)));
    }

    // we need to submit a first read, and this is a good place
    arm_wake_read();
    is_activated_ = true;
}

void IO::submit_or_wait_for()
{
    // Only block if we have no local work to do.

    if (io_uring_cq_ready(&ring_) > 0 || !local_tasks_.empty() || !queue_.empty())
    {
        int ret;
        if (opts_.flags & IORING_SETUP_DEFER_TASKRUN)
        {
            ret = io_uring_submit_and_get_events(&ring_);
        }
        else
        {
            ret = io_uring_submit(&ring_);
        }

        if (ret < 0 && ret != -EINTR)
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

void IO::tick() noexcept
{
    // Drain the cross-thread MPSC queue first.
    queue_.drain(
        [this](detail::TaskPromiseBase* node)
        {
            // We retrieve the safe handle to resume later.
            local_tasks_.push_back(node->self_handle);
        },
        opts_.batch_max_size);

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

void IO::run_blocking(std::stop_token st) noexcept
{
    is_running_ = true;

    pin_to_cpu();

    activate();

    std::stop_callback wake_on_stop{st, [this] { wake(); }};
    while (!st.stop_requested())
    {
        tick();
    }

    is_running_ = false;

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

void IO::pin_to_cpu() const
{
    // TODO: use modulo to map when id_ >= opts_.worker_cpu_affinity.size()
    if (empty(opts_.worker_cpu_affinity) || id_ >= opts_.worker_cpu_affinity.size())
    {
        return;
    }

    int physical_core_id = opts_.worker_cpu_affinity.begin()[id_];

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(physical_core_id, &cpuset);

    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
    {
        ALOG_INFO("Warning: Failed to pin to physical CPU {}: {}", physical_core_id,
                  std::generic_category().message(rc));
    }
}

IO::~IO()
{
    if (ring_.ring_fd > 0 && buffer_pool_.is_registered())
    {
        if (auto res = unregister_buffers(); !res.has_value())
        {
            ALOG_WARN("Failed to unregister buffers: {}", res.error().message());
        }
    }

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
