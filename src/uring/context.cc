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

/// Best effort cancellation request
void IoWorker::request_cancel(const InternalKey key, const uint32_t op_idx) noexcept
{
    auto& op = op_pool_.get(op_idx);
    if (!op.cancel())
    {
        return;
    }

    const auto sqe = get_sqe(key);
    if (sqe == nullptr)
    {
        ALOG_WARN("failed to enqueue cancel operation: SQE queue is full");
        return;
    }

    // Kernel matches EXACT original user_data, preventing stale/race cancels
    io_uring_prep_cancel64(sqe, op.original_ud, 0);
    io_uring_sqe_set_data(sqe, nullptr);
}

void IoWorker::drain_local()
{
    // We swap the vector so that if a resuming coroutine immediately submits
    // a task that completes synchronously (or adds to the queue), it goes into
    // the NEXT tick's batch, preventing an infinite loop inside this tick.
    process_queue_.swap(ready_queue_);

    for (auto h : process_queue_)
    {
        try
        {
            if (h && !h.done())
            {
                h.resume();
            }
        }
        catch (const std::exception& e)
        {
            ALOG_ERROR("coroutine died with error: {}", e.what());
        }
        catch (...)
        {
            ALOG_ERROR("coroutine died with unknown error");
        }
    }
    process_queue_.clear();
}

std::size_t IoWorker::drain_remote_tasks() noexcept
{
    const std::size_t total = spawn_queue_.drain(
        [this](MpscNode* raw) noexcept
        {
            auto* node = static_cast<SpawnNode*>(raw);
            try
            {
                node->task();
            }
            catch (const std::exception& e)
            {
                ALOG_ERROR("remote task died with error: {}", e.what());
            }
            catch (...)
            {
                ALOG_ERROR("remote task died with unknown error");
            }
            release_remote_node(key_, node);
        },
        kMaxRemoteTasksPerTick);

    if (total == kMaxRemoteTasksPerTick)
    {
        wake(key_);
    }

    return total;
}

void IoWorker::tick() noexcept
{
    bool drain_remote_q = false;

    if (!wake_read_armed_)
    {
        arm_wake_read();
    }

    // Only block if we have no local work to do.
    // Local work includes:
    // 1. CQEs already waiting in the ring.
    // 2. Coroutines ready to resume in our ready_queue_.
    // 3. Remote tasks pending in the spawn_queue_ (indicated by wakeup_pending_).
    const bool has_work = io_uring_cq_ready(&ring_) > 0 || !ready_queue_.empty() ||
                          wakeup_pending_.load(std::memory_order_relaxed);

    if (has_work)
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

    // Batch process CQEs
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        count++;
        const auto ud = cqe->user_data;
        if (ud == 0)
        {
            continue;
        }

        if (ud == kWakeTag)
        {
            wake_read_armed_ = false;
            drain_remote_q = true;

            if (cqe->res < 0)
            {
                ALOG_WARN("wake eventfd read failed: {}", std::strerror(-cqe->res));
            }

            arm_wake_read();
            continue;
        }

        if (ud == kRemoteWakeupTag)
        {
            drain_remote_q = true;
            continue;
        }

        if (ud == kRemoteSenderTag)
        {
            if (cqe->res < 0)
            {
                ALOG_ERROR("MSG_RING send failed: {}", std::strerror(-cqe->res));
            }
            continue;
        }

        // Normal I/O completion.
        const auto token = Token::unpack(ud);
        const auto op = op_pool_.try_get(token);
        if (op == nullptr)
        {
            // Stale CQE from a recycled slot - ignore.
            continue;
        }

        op->result_code = cqe->res;
        ready_queue_.push_back(op->handle);
    }

    if (count > 0)
    {
        io_uring_cq_advance(&ring_, count);
    }

    if (drain_remote_q)
    {
        wakeup_pending_.store(false, std::memory_order_release);
        drain_remote_tasks();
    }

    drain_local();
}

void IoWorker::run(InternalKey key, std::stop_token st) noexcept
{
    // owner thread should be set on the thread which start the loop
    owner_thread_ = std::this_thread::get_id();

    // 128-byte frames: 64 preallocated
    // 256-byte frames: most common
    // 512-byte frames: combinators
    CoroAllocator::prewarm(0, 128);
    CoroAllocator::prewarm(1, 256);
    CoroAllocator::prewarm(2, 64);

    std::stop_callback wake_on_stop{st, [this, key] { wake(key); }};
    while (!st.stop_requested())
    {
        tick();
    }

    ALOG_INFO("Worker {} quiescing...", id_);

    ready_queue_.clear();
    process_queue_.clear();
    op_pool_.destroy_active_handles();

    // reset the tls context
    tl_io = nullptr;
    CoroAllocator::cleanup_thread_cache();
}

io_uring_sqe* IoWorker::get_sqe(InternalKey) noexcept
{
    io_uring_sqe* sqe = nullptr;

    for (auto i = 0; i < kMaxSqeGetRetry; ++i)
    {
        sqe = io_uring_get_sqe(&ring_);
        if (sqe != nullptr)
            break;
        io_uring_submit(&ring_);
    }
    assert(sqe != nullptr && "Sqe null after 3 retries, this is a fatal error");
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

void IoWorker::arm_wake_read() noexcept
{
    io_uring_sqe* sqe = get_sqe(key_);

    if (sqe == nullptr)
    {
        ALOG_WARN("failed to get an SQE for wake read, ring queue is full");
        wake_read_armed_ = false;
        return;
    }

    wake_read_armed_ = true;
    io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
    io_uring_sqe_set_data64(sqe, kWakeTag);
}

IoWorker::~IoWorker()
{
    (void)spawn_queue_.drain([](MpscNode* raw) noexcept { delete static_cast<SpawnNode*>(raw); });

    if (ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&ring_);
        ring_.ring_fd = -1;
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
