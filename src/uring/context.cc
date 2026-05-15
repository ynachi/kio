#include "uring/context.h"

#include "cmake-build-debug/_deps/abseil-cpp-src/absl/strings/internal/str_format/extension.h"
#include "cmake-build-trace/_deps/abseil-cpp-src/absl/strings/str_format.h"

#include <cassert>
#include <cstring>
#include <future>

#include <unistd.h>

#include <sys/eventfd.h>

namespace URing
{

void IoWorker::init(const int wq_fd)
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

    ALOG_INFO("Started IO context with {} entries (SQPOLL: {})", opts_.entries,
              (opts_.flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");
}

/// Best effort cancellation request
void IoWorker::request_cancel(const uint32_t op_idx) noexcept
{
    auto& op = op_pool_.get(op_idx);
    if (!op.cancel())
    {
        return;
    }

    const auto sqe = io_uring_get_sqe(&ring_);
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

void IoWorker::tick() noexcept
{
    // Skip the blocking syscall if CQEs are already waiting in the ring
    if (io_uring_cq_ready(&ring_) > 0)
    {
        if (const auto ret = io_uring_submit(&ring_); ret < 0 && ret != -EINTR)
        {
            ALOG_ERROR("failed to submit: {}", std::strerror(-ret));
        }
    }
    else
    {
        // TODO: we need to use the timeout version else, the main loop could hardly be stopped by the stop token in
        // some cases
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
            if (cqe->res < 0)
            {
                ALOG_WARN("wake eventfd read failed: {}", std::strerror(-cqe->res));
            }

            continue;
        }

        const auto token = Token::unpack(ud);
        const auto op = op_pool_.try_get(token);
        if (op == nullptr)
        {
            // Stale CQE from recycled index
            continue;
        }

        op->result_code = cqe->res;
        ready_queue_.push_back(op->handle);
    }

    if (count > 0)
    {
        io_uring_cq_advance(&ring_, count);
    }

    // drain local first, its known as the preferred path
    drain_local();
}

void IoWorker::run(const std::stop_token st) noexcept
{
    // set the context
    tl_io = this;

    // owner thread should be set on the thread which start the loop
    owner_thread_ = std::this_thread::get_id();

    // 128-byte frames: 64 preallocated
    // 256-byte frames: most common
    // 512-byte frames: combinators
    CoroAllocator::prewarm(0, 128);
    CoroAllocator::prewarm(1, 256);
    CoroAllocator::prewarm(2, 64);

    std::stop_callback wake_on_stop{st, [this]
                                    {
                                        // TODO: perform post stop signal stuff here
                                        ALOG_INFO("Io Context stopped");
                                    }};
    while (!st.stop_requested())
    {
        tick();
    }

    // reset the tls context
    tl_io = nullptr;
}

IoWorker::~IoWorker()
{
    if (ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&ring_);
        ring_.ring_fd = -1;
    }
}

//
// OP Context
//
IoContext::IoContext(const std::size_t num_threads, const IoOptions opts) : num_threads_(num_threads), opts_(opts)
{
    contexts_.reserve(num_threads);
    workers_.reserve(num_threads);

    if (num_threads == 0)
    {
        throw std::runtime_error("io context started with 0 thread");
    }

    // Allocate workers on main thread. No rings created yet
    for (std::size_t i = 0; i < num_threads; ++i)
    {
        contexts_.emplace_back(new IoWorker(opts));
    }

    const auto wq_promise = std::make_shared<std::promise<int>>();
    std::shared_future wq_future = wq_promise->get_future();

    for (std::size_t i = 0; i < num_threads; ++i)
    {
        workers_.emplace_back(
            [this, i, wq_promise, wq_future]()
            {
                // TODO: make this configurable Skip CPU 0 for pinning, it SHOULD used for SQPOLL
                IoWorker::pin_to_cpu(static_cast<int>(i + 1));
                // init worker 0 as the ring owner
                if (i == 0)
                {
                    contexts_[i]->init(-1);
                    // share the ring fd to the others
                    wq_promise->set_value(contexts_[i]->ring().ring_fd);
                }
                else
                {
                    // Threads 1..N: Wait for Thread 0 to finish initialization
                    const int owner_fd = wq_future.get();

                    // Initialize secondary rings attached to Thread 0's WQ
                    contexts_[i]->init(owner_fd);
                }

                contexts_[i]->run(stop_source_.get_token());
            });
    }
}
}  // namespace URing
