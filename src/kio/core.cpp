//
// Created by Yao ACHI on 28/01/2026.
//
#include "kio/core/core.hpp"

#include "kio/core/stats.hpp"

#include <liburing/io_uring.h>

#include <cstdio>
#include <cstring>

#include <sys/eventfd.h>
#include <sys/utsname.h>

namespace kio
{
namespace
{
std::optional<std::pair<unsigned, unsigned>> KernelRelease() noexcept
{
    struct utsname uts{};
    if (uname(&uts) != 0)
    {
        return std::nullopt;
    }

    unsigned major = 0;
    unsigned minor = 0;
    if (std::sscanf(uts.release, "%u.%u", &major, &minor) != 2)
    {
        return std::nullopt;
    }

    return std::pair{major, minor};
}

bool KernelAtLeast(const unsigned major, const unsigned minor) noexcept
{
    const auto version = KernelRelease();
    if (!version)
    {
        return false;
    }

    const auto [kernel_major, kernel_minor] = *version;
    return kernel_major > major || (kernel_major == major && kernel_minor >= minor);
}
}  // namespace

// =============================================================================
// Context Tracking (Thread Local)
// =============================================================================

static thread_local void* tl_current_context = nullptr;
static thread_local const void* tl_current_backend_tag = nullptr;

ScopedIoContext::ScopedIoContext(void* ctx, const void* backend_tag)
{
    tl_current_context = ctx;
    tl_current_backend_tag = backend_tag;
}

ScopedIoContext::~ScopedIoContext()
{
    tl_current_context = nullptr;
    tl_current_backend_tag = nullptr;
}

IoContext* IoContext::Current() noexcept
{
    if (tl_current_backend_tag != BackendTag())
    {
        return nullptr;
    }
    return static_cast<IoContext*>(tl_current_context);
}

// =============================================================================
// Core Implementation
// =============================================================================

std::unexpected<IoError> ErrorFromOpenSSL() noexcept
{
    // Pull at least one error
    unsigned long e = ERR_get_error();
    if (e == 0)
    {
        return ErrorFromErrc(std::errc::protocol_error);
    }

    // Drain remaining errors; keep the last (often most informative)
    unsigned long last = e;
    while ((e = ERR_get_error()) != 0)
    {
        last = e;
    }

    return std::unexpected(IoError::FromOpenSSL(last));
}

void UringBackend::Init(const unsigned entries)
{
    io_uring_params params{};
    params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    setup_flags_ = params.flags;

    if (const int ret = io_uring_queue_init_params(entries, &ring_, &params); ret < 0)
    {
        throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params");
    }

    capabilities_.ring_resize = KernelAtLeast(6, 13) && (setup_flags_ & IORING_SETUP_DEFER_TASKRUN);

    wake_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wake_fd_ < 0)
    {
        io_uring_queue_exit(&ring_);
        throw std::system_error(errno, std::system_category(), "eventfd");
    }

    SubmitWakeRead();
    io_uring_submit(&ring_);
}

void UringBackend::Shutdown() noexcept
{
    if (wake_fd_ >= 0)
    {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
    io_uring_queue_exit(&ring_);
}

bool UringBackend::Notify() const noexcept
{
    if (wake_fd_ == -1)
    {
        return false;
    }
    constexpr uint64_t val = 1;
    return ::write(wake_fd_, &val, sizeof(val)) == sizeof(val);
}

int UringBackend::SubmitAndWait(const unsigned wait_nr)
{
    int ret = 0;
    do
    {
        ret = io_uring_submit_and_wait(&ring_, wait_nr);
    } while (ret == -EINTR);

    return ret;
}

bool UringBackend::TryMsgRing(const UringBackend& target, OperationState* op)
{
    EnsureSqes(1);
    auto* sqe = GetSqe();
    if (sqe == nullptr)
    {
        return false;
    }

    io_uring_prep_msg_ring(sqe, target.RingFd(), 0, reinterpret_cast<uint64_t>(op), 0);
    io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
    io_uring_sqe_set_data64(sqe, 0);
    return true;
}

void UringBackend::CancelAllPending()
{
    io_uring_sqe* sqe = GetSqe();
    if (sqe == nullptr)
    {
        return;
    }

    io_uring_prep_cancel(sqe, nullptr, IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    io_uring_sqe_set_data(sqe, nullptr);
    io_uring_submit(&ring_);
}

Result<> UringBackend::RegisterFiles(const std::span<const int> fds)
{
    if (const int ret = io_uring_register_files(&ring_, fds.data(), fds.size()); ret < 0)
    {
        return ErrorFromErrno(-ret);
    }
    return {};
}

Result<> UringBackend::Resize(const unsigned entries, const std::optional<unsigned> cq_entries)
{
    if (entries == 0)
    {
        return ErrorFromErrc(std::errc::invalid_argument);
    }
    if (cq_entries.has_value() && (*cq_entries == 0 || *cq_entries < entries))
    {
        return ErrorFromErrc(std::errc::invalid_argument);
    }

    if (!capabilities_.ring_resize)
    {
        return ErrorFromErrc(std::errc::operation_not_supported);
    }

    io_uring_params params{};
    params.flags = IORING_SETUP_CLAMP;
    params.sq_entries = entries;

    if (cq_entries.has_value())
    {
        params.flags |= IORING_SETUP_CQSIZE;
        params.cq_entries = *cq_entries;
    }

    const int ret = io_uring_resize_rings(&ring_, &params);
    if (ret < 0)
    {
        if (ret == -EINVAL || ret == -EOPNOTSUPP)
        {
            return ErrorFromErrc(std::errc::operation_not_supported);
        }
        return ErrorFromErrno(-ret);
    }

    return {};
}

void UringBackend::EnsureSqes(const unsigned n)
{
    if (io_uring_sq_space_left(&ring_) < n)
    {
        io_uring_submit(&ring_);

        if (io_uring_sq_space_left(&ring_) < n)
        {
            throw std::runtime_error("SQ full after submit");
        }
    }
}

io_uring_sqe* UringBackend::GetSqe()
{
    return io_uring_get_sqe(&ring_);
}

void UringBackend::SubmitWakeRead()
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
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    io_uring_sqe_set_data64(sqe, detail::WAKE_TAG);
}

void UringBackend::FlushAfterWake()
{
    (void)io_uring_submit(&ring_);
}

IoContext::IoContext(const unsigned entries)
{
    ready_.reserve(entries);
    backend_.Init(entries);

    ready_latch_.count_down();
}

IoContext::IoContext(UringBackend backend, const unsigned entries) : backend_(std::move(backend))
{
    ready_.reserve(entries);
    backend_.Init(entries);

    ready_latch_.count_down();
}

IoContext::~IoContext() noexcept
{
    CancelAllPending();
    backend_.Shutdown();
}

bool IoContext::Notify() const noexcept
{
    return backend_.Notify();
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

    // If op was scheduled via MsgRing or External, it won't be in the
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
    if (pending_head_ == nullptr)
    {
        return;
    }

    // mark all ops
    for (auto* op = pending_head_; op != nullptr; op = op->next)
    {
        op->cancel_reason = reason;
    }

    backend_.CancelAllPending();

    DrainWithoutResume();
}

Result<> IoContext::RegisterFiles(const std::span<const int> fds)
{
    AssertOwnerThread();
    return backend_.RegisterFiles(fds);
}

Result<> IoContext::Resize(const unsigned entries, const std::optional<unsigned> cq_entries)
{
    AssertOwnerThread();

    if (auto result = backend_.Resize(entries, cq_entries); !result.has_value())
    {
        return result;
    }

    if (ready_.capacity() < backend_.SqEntries())
    {
        ready_.reserve(backend_.SqEntries());
    }
    return {};
}

// -----------------------------------------------------------------------------
// Cross-Thread Scheduling Logic
// -----------------------------------------------------------------------------

bool IoContext::TryMsgRing(const IoContext& target, OperationState* op)
{
    return backend_.TryMsgRing(target.GetBackend(), op);
}

void IoContext::SubmitExternal(OperationState* op)
{
    OperationState* old_head = ext_submission_head_.load(std::memory_order_relaxed);
    do
    {
        op->next_ext.store(old_head, std::memory_order_relaxed);
    } while (!ext_submission_head_.compare_exchange_weak(old_head, op, std::memory_order_release,
                                                         std::memory_order_relaxed));

    // Only write if consumer is likely sleeping
    if (!ext_hint_.exchange(true, std::memory_order_release))
    {
        (void)Notify();
    }
}

// -----------------------------------------------------------------------------
// Loop & Step
// -----------------------------------------------------------------------------

void IoContext::DrainExternal(std::vector<std::coroutine_handle<>>& out)
{
    // Clear hint
    ext_hint_.store(false, std::memory_order_release);

    // Steal list
    OperationState* head = ext_submission_head_.exchange(nullptr, std::memory_order_acquire);

    if (head == nullptr)
    {
        return;
    }

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

    if (head == nullptr)
    {
        return;
    }

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
    if (count > 0)
    {
        AIO_STATS_ADD(stats_, external_completions, count);
    }
#endif
}

void IoContext::DrainWithoutResume()
{
    while (pending_head_ != nullptr)
    {
        const bool got_completion = backend_.DrainWithoutResume(
            [&](const uint64_t user_data)
            {
                if (user_data == detail::WAKE_TAG)
                {
                    DrainExternalWithoutResume();
                    SubmitWakeRead();
                    backend_.FlushAfterWake();
                }
                else if (user_data != 0)
                {
                    auto* op = reinterpret_cast<OperationState*>(static_cast<uintptr_t>(user_data));
                    Untrack(op);
                }
            });

        if (!got_completion)
        {
            break;
        }
    }
}

void IoContext::SubmitWakeRead()
{
    backend_.SubmitWakeRead();
}

void IoContext::Step()
{
    if (const int ret = backend_.SubmitAndWait(1); ret < 0)
    {
        return;
    }

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
        if (h && !h.done())
        {
            h.resume();
        }
    }
}

std::pair<unsigned, bool> IoContext::ProcessReadyCompletions()
{
    return backend_.ProcessReadyCompletions(
        [this](OperationState* op, const int32_t res)
        {
            Untrack(op);
            op->res = res;
            ready_.push_back(op->handle);

#if AIO_STATS
            AIO_STATS_INC(stats_, ops_completed);
            if (res < 0)
            {
                AIO_STATS_INC(stats_, ops_errors);
            }
#endif
        });
}

SignalSet::SignalSet(const std::initializer_list<int> sigs) : fd_(eventfd(0, EFD_CLOEXEC))
{
    if (fd_ < 0)
    {
        throw std::system_error(errno, std::system_category(), "eventfd");
    }
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
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
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
    if (res < 0)
    {
        return std::unexpected(make_error_code(res));
    }
    return static_cast<int>(signo);
}

}  // namespace kio
