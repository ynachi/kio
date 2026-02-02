//
// Created by Yao ACHI on 28/01/2026.
//
#include "../../include/aio/core/core.hpp"

#include <liburing/io_uring.h>

#include <sys/eventfd.h>

#include "../../include/aio/core/stats.hpp"

namespace aio
{
std::unexpected<std::error_code> ErrorFromOpenSSL() noexcept
{
    // Pull at least one error
    unsigned long e = ERR_get_error();
    if (e == 0)
    {
        return std::unexpected(std::make_error_code(std::errc::protocol_error));
    }

    // Drain remaining errors; keep the last (often most informative)
    unsigned long last = e;
    while ((e = ERR_get_error()) != 0)
    {
        last = e;
    }

    const auto bits = static_cast<uint32_t>(last);
    const int ev = std::bit_cast<int>(bits);

    return std::unexpected(std::error_code(ev, detail::openssl_category()));
}

////////////////////////////////////////////////////////////////////////////////
// Io Context
//
// Core IO construct for kio, a single threaded io_uring wrapper
////////////////////////////////////////////////////////////////////////////////
IoContext::IoContext(const unsigned entries)
{
    io_uring_params params{};

    params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    if (const int ret = io_uring_queue_init_params(entries, &ring_, &params); ret < 0)
    {
        throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params");
    }

    // Reduce allocations in the hot path.
    ready_.reserve(entries);
    ext_done_.reserve(entries);

    // Initialize internal wake eventfd
    wake_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wake_fd_ < 0)
    {
        io_uring_queue_exit(&ring_);
        throw std::system_error(errno, std::system_category(), "eventfd");
    }

    // Submit the initial read on the wake_fd
    SubmitWakeRead();
    io_uring_submit(&ring_);

    // signal that we are up
    ready_latch_.count_down();
}

bool IoContext::Notify() const noexcept
{
    if (wake_fd_ == -1)
    {
        return false;
    }
    constexpr uint64_t val = 1;
    // Direct write to eventfd is thread-safe and doesn't touch the ring
    return ::write(wake_fd_, &val, sizeof(val)) == sizeof(val);
}

void IoContext::Track(OperationState* op)
{
    AssertOwnerThread();

    op->tracked = true;
    op->next = pending_head_;
    op->prev = nullptr;
    if (pending_head_ != nullptr)
    {
        pending_head_->prev = op;
    }
    pending_head_ = op;

#if AIO_STATS
    AIO_STATS_INC(stats_, ops_submitted);
    const uint64_t inflight = AIO_STATS_ADD(stats_, ops_inflight, 1) + 1;
    AIO_STATS_SET_MAX(stats_, ops_max_inflight, inflight);
#endif
}

void IoContext::Untrack(OperationState* op)
{
    AssertOwnerThread();

    if (op->prev != nullptr)
    {
        op->prev->next = op->next;
    }
    else if (pending_head_ == op)
    {
        pending_head_ = op->next;
    }
    if (op->next)
    {
        op->next->prev = op->prev;
    }
    op->next = nullptr;
    op->prev = nullptr;
    op->tracked = false;

#if AIO_STATS
    AIO_STATS_DEC(stats_, ops_inflight);
#endif
}

void IoContext::CancelAllPending(const OpCancelReason reason)
{
    // collect all operations to cancel first
    // TODO: is it necessary to collect first for safety ?
    std::vector<OperationState*> ops_to_cancel;
    for (auto* op = pending_head_; op != nullptr; op = op->next)
    {
        ops_to_cancel.push_back(op);
    }

    // Submit cancel requests for all tracked operations
    for (auto* op : ops_to_cancel)
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                break;
            }
        }
        op->cancel_reason = reason;
        io_uring_prep_cancel(sqe, op, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }
    io_uring_submit(&ring_);

    // Drain until all operations are untracked
    DrainWithoutResume();
}

Result<> IoContext::RegisterFiles(const std::span<const int> fds)
{
    AssertOwnerThread();

    if (const int ret = io_uring_register_files(&ring_, fds.data(), fds.size()); ret < 0)
    {
        return ErrorFromErrno(-ret);
    }
    return {};
}

bool IoContext::EnqueueExternalDone(OperationState* op)
{
    std::scoped_lock lk(ext_mtx_);
    ext_done_.push_back(op);
    ext_hint_.store(true, std::memory_order_relaxed);
    if (!ext_wake_pending_)
    {
        ext_wake_pending_ = true;
        return true;
    }
    return false;
}

void IoContext::EnsureSqes(const unsigned n)
{
    AssertOwnerThread();

    if (io_uring_sq_space_left(&ring_) < n)
    {
        io_uring_submit(&ring_);

        if (io_uring_sq_space_left(&ring_) < n)
        {
            throw std::runtime_error("SQ full after submit");
        }
    }
}

void IoContext::DrainExternal(std::vector<std::coroutine_handle<>>& out)
{
#if AIO_STATS
    uint64_t external_count = 0;
#endif

    std::vector<OperationState*> local;
    {
        std::scoped_lock lk(ext_mtx_);
        if (ext_done_.empty())
        {
            ext_wake_pending_ = false;
            ext_hint_.store(false, std::memory_order_relaxed);
            return;
        }
        local.swap(ext_done_);
        ext_wake_pending_ = false;
        ext_hint_.store(false, std::memory_order_relaxed);
    }

    for (auto* op : local)
    {
        if (op == nullptr)
        {
            continue;
        }
#if AIO_STATS
        external_count++;
        AIO_STATS_INC(stats_, ops_completed);
#endif
        Untrack(op);
        out.push_back(op->handle);
    }

#if AIO_STATS
    if (external_count != 0)
    {
        AIO_STATS_ADD(stats_, external_completions, external_count);
    }
#endif
}

void IoContext::DrainExternalWithoutResume()
{
    if (!ext_hint_.load(std::memory_order_relaxed))
    {
        return;
    }

#if AIO_STATS
    uint64_t external_count = 0;
#endif

    std::vector<OperationState*> local;
    {
        std::scoped_lock lk(ext_mtx_);
        if (ext_done_.empty())
        {
            ext_wake_pending_ = false;
            ext_hint_.store(false, std::memory_order_relaxed);
            return;
        }
        local.swap(ext_done_);
        ext_wake_pending_ = false;
        ext_hint_.store(false, std::memory_order_relaxed);
    }

    for (auto* op : local)
    {
        if (op == nullptr)
        {
            continue;
        }
#if AIO_STATS
        external_count++;
        AIO_STATS_INC(stats_, ops_completed);
#endif
        Untrack(op);
        op->handle = {};
    }

#if AIO_STATS
    if (external_count != 0)
    {
        AIO_STATS_ADD(stats_, external_completions, external_count);
    }
#endif
}

void IoContext::DrainWithoutResume()
{
    while (pending_head_ != nullptr)
    {
        io_uring_cqe* cqe = nullptr;
        // Retry on EINTR
        int ret = 0;
        do
        {
            ret = io_uring_wait_cqe(&ring_, &cqe);
        } while (ret == -EINTR);

        if (ret < 0)
        {
            // Unrecoverable error in destruction path
            break;
        }

        const auto ud = io_uring_cqe_get_data64(cqe);

        if (ud == detail::WAKE_TAG)
        {
            // A cross-thread wake. Drain any externally completed ops.
            DrainExternalWithoutResume();
            SubmitWakeRead();
            (void)io_uring_submit(&ring_);
        }
        else if (ud)
        {
            auto* op = reinterpret_cast<OperationState*>(static_cast<uintptr_t>(ud));
#if AIO_STATS
            AIO_STATS_INC(stats_, ops_completed);
            if (cqe->res < 0)
            {
                AIO_STATS_INC(stats_, ops_errors);
            }
#endif
            Untrack(op);
            // Do NOT resume op->handle — we're draining, not running
        }
        io_uring_cqe_seen(&ring_, cqe);
    }
}

void IoContext::SubmitWakeRead()
{
    // Must ensure we have space, though inside Step we typically do.
    // We use a simple read on the eventfd.
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        // Force flush if full? This is rare in typical loop usage.
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            // Should fatal error really // TODO fix this kater, not good right now
            ALOG_ERROR("failed to get io completion, probably resource unavailable");
            return;
        }
    }

    io_uring_prep_read(sqe, wake_fd_, &wake_buffer_, sizeof(wake_buffer_), 0);
    io_uring_sqe_set_data64(sqe, detail::WAKE_TAG);
}

int IoContext::SubmitSqesWait(const uint32_t wait_us)
{
    // Define the heartbeat interval (max sleep time)
    __kernel_timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = wait_us * 1000;

    // DYNAMIC BUSY WAIT:
    // Only engage the kernel-side busy loop if we are actually submitting new work.
    // If we are just checking for completions (idle/heartbeat), sleep immediately.
    // TODO: make as config
    unsigned min_wait = 20;

    if (io_uring_sq_ready(&ring_) == 0)
    {
        min_wait = 0;
    }

    io_uring_cqe* cqe_ptr = nullptr;

    int ret = 0;
    do
    {
        ret = io_uring_submit_and_wait_min_timeout(&ring_, &cqe_ptr, 1, &ts, min_wait, nullptr);
    } while (ret == -EINTR);

    return ret;
}

void IoContext::Step()
{
    // Retry on EINTR
    // TODO as config
    if (const int ret = SubmitSqesWait(100); ret < 0)
    {
        // If we failed to wait (and it wasn't EINTR), we can't really proceed.
        // Returning here might spin the loop if the error persists,
        // // but throwing from Step() is also
        // aggressive. For now, we assume transient errors or fatal ones we can't fix.
        return;
    }

#if AIO_STATS
    AIO_STATS_INC(stats_, loop_iterations);
#endif

    ready_.clear();

    auto [_, saw_wake] = ProcessReadyCompletions();

    // If a pool thread (or any other producer) completed work for this
    // context, it will have pushed ops into ext_done_ and signaled WakeFd.
    if (saw_wake || ext_hint_.load(std::memory_order_relaxed))
    {
        DrainExternal(ready_);
    }

#if AIO_STATS
    if (saw_wake)
    {
        AIO_STATS_INC(stats_, loop_wakeups);
    }
    const auto batch = ready_.size();
    if (batch == 0)
    {
        AIO_STATS_INC(stats_, loop_idle_iterations);
    }
    else
    {
        AIO_STATS_INC(stats_, loop_busy_iterations);
        AIO_STATS_ADD(stats_, loop_completions, batch);
        AIO_STATS_SET_MAX(stats_, loop_max_batch, batch);
    }
#endif

    // Resume outside CQE iteration (flat, no stack growth)
    for (auto h : ready_)
    {
        if (h && !h.done())
        {
            h.resume();
        }
    }
}

std::pair<unsigned, bool> IoContext::ProcessReadyCompletions()
{
    io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;
    bool saw_wake = false;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        count++;
        const auto user_data = io_uring_cqe_get_data64(cqe);

        if (user_data == 0)
        {
            continue;
        }

        if (user_data == detail::WAKE_TAG)
        {
            saw_wake = true;
            // Re-arm the wake mechanism immediately for next wait
            SubmitWakeRead();
            continue;
        }

        auto* op = reinterpret_cast<OperationState*>(static_cast<uintptr_t>(user_data));
        Untrack(op);
        op->res = cqe->res;
        ready_.push_back(op->handle);
#if AIO_STATS
        AIO_STATS_INC(stats_, ops_completed);
        if (cqe->res < 0)
        {
            AIO_STATS_INC(stats_, ops_errors);
        }
#endif
    }

    io_uring_cq_advance(&ring_, count);

    return {count, saw_wake};
}

}  // namespace aio