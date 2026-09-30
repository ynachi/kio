#include "uring/core/io.h"

#include <algorithm>
#include <cerrno>
#include <stdexcept>

#include <sys/eventfd.h>

namespace URing
{
IO::IO(const size_t id, const IO* leader, const IoOptions& opts, std::initializer_list<BucketConfig> buffers)
    : opts_(opts), id_(id), buffer_pool_(buffers)
{
    if (opts_.batch_max_size == 0)
        throw std::invalid_argument("batch_max_size must be positive");
    io_uring_params params{};
    params.flags = opts_.flags | IORING_SETUP_R_DISABLED;
    if (leader)
    {
        params.flags |= IORING_SETUP_ATTACH_WQ;
        params.wq_fd = leader->ring_.ring_fd;
    }
    if (params.flags & IORING_SETUP_SQPOLL)
    {
        params.sq_thread_idle = opts_.sq_thread_idle_ms;
        if (opts_.sq_thread_cpu >= 0)
        {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = opts_.sq_thread_cpu;
        }
    }
    if (const int ret = io_uring_queue_init_params(opts_.entries, &ring_, &params); ret < 0)
        throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params");
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0)
    {
        const int error = errno;
        io_uring_queue_exit(&ring_);
        throw std::system_error(error, std::system_category(), "eventfd");
    }
    if (buffer_pool_.total_capacity() != 0)
    {
        if (const int ret = io_uring_register_buffers(&ring_, buffer_pool_.iovecs_ptr(), buffer_pool_.total_capacity());
            ret < 0)
        {
            io_uring_queue_exit(&ring_);
            ::close(wake_fd_);
            throw std::system_error(-ret, std::system_category(), "io_uring_register_buffers");
        }
    }
}

bool IO::schedule(Task<void> task)
{
    std::scoped_lock lock(admission_mutex_);
    if (!accepting_)
        return false;
    const auto handle = task.release();
    if (!handle)
        throw std::invalid_argument("cannot schedule an empty Task");
    auto& promise = handle.promise();
    if (promise.started || promise.owner)
        std::terminate();
    promise.owner = this;
    promise.self_handle = handle;
    incoming_.enqueue(&promise);
    wake();
    return true;
}

void IO::assert_owner() const
{
    if (owner_thread_ != std::this_thread::get_id())
        std::terminate();
}

void IO::activate()
{
    if (activated_)
    {
        assert_owner();
        return;
    }
    owner_thread_ = std::this_thread::get_id();
    if (id_ < opts_.worker_cpu_affinity.size())
    {
        const int cpu = opts_.worker_cpu_affinity[id_];
        if (cpu < 0 || cpu >= CPU_SETSIZE)
            throw std::invalid_argument("invalid worker CPU");
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(cpu, &cpus);
        if (const int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus); ret != 0)
            throw std::system_error(ret, std::system_category(), "pthread_setaffinity_np");
    }
    if (const int ret = io_uring_register(ring_.ring_fd, IORING_REGISTER_ENABLE_RINGS, nullptr, 0); ret < 0)
        throw std::system_error(-ret, std::system_category(), "enable io_uring");
    activated_ = true;
    arm_wake_read();
}

Result<FixedBuffer> IO::take_fixed_buffer(size_t size)
{
    assert_owner();
    return buffer_pool_.take(size);
}

void IO::adopt_roots()
{
    incoming_.drain(
        [this](detail::TaskPromiseBase* root)
        {
            ++active_roots_;
            root->on_complete = complete_root;
            ready_.push_back(root->self_handle);
        },
        opts_.batch_max_size);
}

void IO::complete_root(detail::TaskPromiseBase& root) noexcept
{
    IO& io = *root.owner;
    --io.active_roots_;
    root.root_next = io.completed_;
    io.completed_ = &root;
}

void IO::reap_completed() noexcept
{
    while (completed_)
    {
        auto* root = completed_;
        completed_ = root->root_next;
        root->self_handle.destroy();
    }
}

void IO::track(IoOps& op) noexcept
{
    op.next = pending_;
    if (pending_)
        pending_->prev = &op;
    pending_ = &op;
    op.pending = true;
}

void IO::untrack(IoOps& op) noexcept
{
    if (op.prev)
        op.prev->next = op.next;
    else
        pending_ = op.next;
    if (op.next)
        op.next->prev = op.prev;
    op.prev = op.next = nullptr;
    op.pending = false;
}

void IO::tick()
{
    assert_owner();
    adopt_roots();
    if (!wake_armed_)
        arm_wake_read();
    int ret = 0;
    do
    {
        if (!ready_.empty() || !incoming_.empty() || io_uring_cq_ready(&ring_) > 0)
            ret = io_uring_submit_and_get_events(&ring_);
        else
            ret = io_uring_submit_and_wait(&ring_, 1);
    } while (ret == -EINTR);
    if (ret < 0)
        throw std::system_error(-ret, std::system_category(), "submit io_uring");
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0, count = 0;
    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        ++count;
        if (cqe->user_data == kWakeTag)
            wake_armed_ = false;
        else if (cqe->user_data != 0)
        {
            auto& op = *static_cast<IoOps*>(io_uring_cqe_get_data(cqe));
            untrack(op);
            op.res = cqe->res;
            ready_.push_back(op.h);
        }
    }
    io_uring_cq_advance(&ring_, count);
    // Work enqueued by a continuation waits for a later tick. All executions
    // obey one budget, whether they originate from submission or completion.
    const size_t batch = std::min(opts_.batch_max_size, ready_.size());
    for (size_t i = 0; i < batch; ++i)
    {
        auto handle = ready_.front();
        ready_.pop_front();
        handle.resume();
    }
    reap_completed();
}

void IO::cancel_pending()
{
    for (auto* op = pending_; op; op = op->next)
    {
        if (!op->cancelable || op->cancel_requested)
            continue;
        auto* sqe = get_sqe();
        if (!sqe)
            break;
        io_uring_prep_cancel(sqe, op, 0);
        io_uring_sqe_set_data(sqe, nullptr);
        op->cancel_requested = true;
    }
}

void IO::shutdown()
{
    {
        std::scoped_lock lock(admission_mutex_);
        accepting_ = false;
    }
    stopping_ = true;
    do
    {
        adopt_roots();
        cancel_pending();
        if (active_roots_ || pending_ || !ready_.empty())
            tick();
    } while (active_roots_ || pending_ || !ready_.empty() || !incoming_.empty());
    reap_completed();
}

void IO::run(std::stop_token stop)
{
    if (running_ || stopping_)
        throw std::logic_error("IO::run cannot restart");
    activate();
    running_ = true;
    std::stop_callback wake_on_stop{stop, [this] { wake(); }};
    try
    {
        while (!stop.stop_requested())
            tick();
        shutdown();
        running_ = false;
    }
    catch (...)
    {
        shutdown();
        running_ = false;
        throw;
    }
}

io_uring_sqe* IO::get_sqe() noexcept
{
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
}

void IO::arm_wake_read()
{
    if (auto* sqe = get_sqe())
    {
        io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
        io_uring_sqe_set_data64(sqe, kWakeTag);
        wake_armed_ = true;
    }
}

void IO::wake() const noexcept
{
    const uint64_t one = 1;
    while (::write(wake_fd_, &one, sizeof(one)) < 0 && errno == EINTR)
    {
    }
}

IO::~IO()
{
    if (running_)
        std::terminate();
    if (activated_ && !stopping_)
    {
        assert_owner();
        shutdown();
    }
    else
    {
        std::scoped_lock lock(admission_mutex_);
        accepting_ = false;
        incoming_.drain([](const detail::TaskPromiseBase* root) { root->self_handle.destroy(); });
    }
    // Ring teardown precedes freeing registered memory or the wake-read buffer.
    io_uring_queue_exit(&ring_);
    ::close(wake_fd_);
}
}  // namespace URing
