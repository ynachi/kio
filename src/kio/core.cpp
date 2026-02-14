//
// Created by Yao ACHI on 28/01/2026.
//
#include "kio/core/core.hpp"

#include <liburing/io_uring.h>

#include <sys/eventfd.h>

#include "kio/core/stats.hpp"

namespace kio
{
    // =============================================================================
    // Context Tracking (Thread Local)
    // =============================================================================

    static thread_local IoContext* tl_current_context = nullptr;

    ScopedIoContext::ScopedIoContext(IoContext* ctx)
    {
        tl_current_context = ctx;
    }

    ScopedIoContext::~ScopedIoContext()
    {
        tl_current_context = nullptr;
    }

    IoContext* IoContext::Current() noexcept
    {
        return tl_current_context;
    }

    // =============================================================================
    // Core Implementation
    // =============================================================================

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

    IoContext::IoContext(const unsigned entries)
    {
        io_uring_params params{};

        params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

        if (const int ret = io_uring_queue_init_params(entries, &ring_, &params); ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params");
        }

        ready_.reserve(entries);

        wake_fd_ = eventfd(0, EFD_CLOEXEC);
        if (wake_fd_ < 0)
        {
            io_uring_queue_exit(&ring_);
            throw std::system_error(errno, std::system_category(), "eventfd");
        }

        SubmitWakeRead();
        io_uring_submit(&ring_);

        ready_latch_.count_down();
    }

    IoContext::~IoContext() noexcept
    {
        CancelAllPending();
        if (wake_fd_ >= 0)
        {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
        io_uring_queue_exit(&ring_);
    }

    bool IoContext::Notify() const noexcept
    {
        if (wake_fd_ == -1)
        {
            return false;
        }
        constexpr uint64_t val = 1;
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

        // SAFETY: If op was scheduled via MsgRing or External, it won't be in the
        // linked list. We check 'tracked' to avoid corruption.
        if (!op->tracked)
        {
            return;
        }

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
        std::vector<OperationState*> ops_to_cancel;
        for (auto* op = pending_head_; op != nullptr; op = op->next)
        {
            ops_to_cancel.push_back(op);
        }

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

    // -----------------------------------------------------------------------------
    // Cross-Thread Scheduling Logic
    // -----------------------------------------------------------------------------

    bool IoContext::TryMsgRing(IoContext& target, OperationState* op)
    {
        // 1. Get SQE from current ring
        EnsureSqes(1);
        io_uring_sqe* sqe = GetSqe();

        // 2. Prep MSG_RING
        // target_fd = target.RingFd()
        // len = 0 (res field of target CQE)
        // data = op (user_data field of target CQE)
        // flags = 0
        io_uring_prep_msg_ring(sqe, target.RingFd(), 0, reinterpret_cast<uint64_t>(op), 0);

        // 3. Fire and forget the sender-side completion
        io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
        io_uring_sqe_set_data64(sqe, 0);

        return true;
    }

    void IoContext::SubmitExternal(OperationState* op)
    {
        // Atomic Push: Add to the lock-free intrusive stack
        OperationState* old_head = ext_submission_head_.load(std::memory_order_relaxed);
        do
        {
            op->next_ext.store(old_head, std::memory_order_relaxed);
        }
        while (!ext_submission_head_.compare_exchange_weak(old_head, op, std::memory_order_release,
                                                           std::memory_order_relaxed));

        // Notification Logic: Only write if consumer is likely sleeping
        if (!ext_hint_.exchange(true, std::memory_order_release))
        {
            (void)Notify();
        }
    }

    // -----------------------------------------------------------------------------
    // Loop & Step
    // -----------------------------------------------------------------------------

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
        // Clear hint
        ext_hint_.store(false, std::memory_order_release);

        // Steal list
        OperationState* head = ext_submission_head_.exchange(nullptr, std::memory_order_acquire);

        if (head == nullptr) return;

        // Reverse LIFO -> FIFO
        OperationState* prev = nullptr;
        OperationState* curr = head;
        while (curr)
        {
            OperationState* next = curr->next_ext.load(std::memory_order_relaxed);
            curr->next_ext.store(prev, std::memory_order_relaxed);
            prev = curr;
            curr = next;
        }
        head = prev;

        // Process
        uint64_t count = 0;
        while (head)
        {
            Untrack(head);
            out.push_back(head->handle);
            head = head->next_ext.load(std::memory_order_relaxed);
            count++;
        }

#if AIO_STATS
        if (count > 0)
        {
            AIO_STATS_ADD(stats_, external_completions, count);
            AIO_STATS_ADD(stats_, ops_completed, count);
        }
#endif
    }

    void IoContext::DrainExternalWithoutResume()
    {
        ext_hint_.store(false, std::memory_order_release);
        OperationState* head = ext_submission_head_.exchange(nullptr, std::memory_order_acquire);

        if (head == nullptr) return;

        OperationState* prev = nullptr;
        OperationState* curr = head;
        while (curr)
        {
            OperationState* next = curr->next_ext.load(std::memory_order_relaxed);
            curr->next_ext.store(prev, std::memory_order_relaxed);
            prev = curr;
            curr = next;
        }
        head = prev;

        uint64_t count = 0;
        while (head)
        {
            Untrack(head);
            head->handle = {};
            head = head->next_ext.load(std::memory_order_relaxed);
            count++;
        }

#if AIO_STATS
        if (count > 0) { AIO_STATS_ADD(stats_, external_completions, count); }
#endif
    }

    void IoContext::DrainWithoutResume()
    {
        while (pending_head_ != nullptr)
        {
            io_uring_cqe* cqe = nullptr;
            int ret = 0;
            do { ret = io_uring_wait_cqe(&ring_, &cqe); }
            while (ret == -EINTR);

            if (ret < 0) break;

            const auto ud = io_uring_cqe_get_data64(cqe);

            if (ud == detail::WAKE_TAG)
            {
                DrainExternalWithoutResume();
                SubmitWakeRead();
                (void)io_uring_submit(&ring_);
            }
            else if (ud)
            {
                auto* op = reinterpret_cast<OperationState*>(static_cast<uintptr_t>(ud));
                Untrack(op);
            }
            io_uring_cqe_seen(&ring_, cqe);
        }
    }

    void IoContext::SubmitWakeRead()
    {
        auto* sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
        {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                ALOG_ERROR("failed to get io completion, probably resource unavailable");
                return;
            }
        }
        io_uring_prep_read(sqe, wake_fd_, &wake_buffer_, sizeof(wake_buffer_), 0);
        io_uring_sqe_set_data64(sqe, detail::WAKE_TAG);
    }

    void IoContext::Step()
    {
        int ret = 0;
        do { ret = io_uring_submit_and_wait(&ring_, 1); }
        while (ret == -EINTR);

        if (ret < 0) return;

#if AIO_STATS
        AIO_STATS_INC(stats_, loop_iterations);
#endif

        ready_.clear();

        auto [_, saw_wake] = ProcessReadyCompletions();

        if (saw_wake || ext_hint_.load(std::memory_order_relaxed))
        {
            DrainExternal(ready_);
        }

#if AIO_STATS
        // ... stats logic ...
#endif

        for (auto h : ready_)
        {
            if (h && !h.done()) h.resume();
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

            if (user_data == 0) continue;

            if (user_data == detail::WAKE_TAG)
            {
                saw_wake = true;
                SubmitWakeRead();
                continue;
            }

            // Handle normal IO OR MSG_RING injection
            auto* op = reinterpret_cast<OperationState*>(static_cast<uintptr_t>(user_data));
            Untrack(op); // Safe due to safety check in Untrack
            op->res = cqe->res;
            ready_.push_back(op->handle);

#if AIO_STATS
            AIO_STATS_INC(stats_, ops_completed);
            if (cqe->res < 0)
                AIO_STATS_INC(stats_, ops_errors);
#endif
        }

        io_uring_cq_advance(&ring_, count);
        return {count, saw_wake};
    }

    // ... SignalHandler ...

    SignalSet::SignalSet(const std::initializer_list<int> sigs) : fd_(eventfd(0, EFD_CLOEXEC))
    {
        if (fd_ < 0) throw std::system_error(errno, std::system_category(), "eventfd");
        instance_.store(this, std::memory_order_release);

        struct sigaction sa{};
        sa.sa_handler = SignalHandler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);

        for (const int sig : sigs)
        {
            struct sigaction old_action{};
            if (sigaction(sig, &sa, &old_action) < 0)
            {
                instance_.store(nullptr, std::memory_order_release);
                ::close(fd_);
                throw std::system_error(errno, std::system_category(), "sigaction");
            }
            old_actions_.emplace_back(sig, old_action);
        }
    }

    SignalSet::~SignalSet()
    {
        for (const auto& [sig, old_action] : old_actions_)
        {
            sigaction(sig, &old_action, nullptr);
        }
        instance_.store(nullptr, std::memory_order_release);
        if (fd_ >= 0) ::close(fd_);
    }

    void SignalSet::SignalHandler(int sig)
    {
        if (const SignalSet* self = instance_.load(std::memory_order_acquire))
        {
            const auto val = static_cast<uint64_t>(sig);
            (void)::write(self->fd_, &val, sizeof(val));
        }
    }

    Result<int> WaitSignalOp::await_resume()
    {
        if (res < 0) return std::unexpected(make_error_code(res));
        return static_cast<int>(signo);
    }
} // namespace kio
