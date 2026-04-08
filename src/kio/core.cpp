//
// Created by Yao ACHI on 28/01/2026.
//
#include "kio/core/core.hpp"

#include "kio/core/stats.hpp"

#include <liburing/io_uring.h>

#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>

namespace kio
{
namespace
{
using TimerQueue =
    std::priority_queue<kio::MemoryBackend::TimerEntry, std::vector<kio::MemoryBackend::TimerEntry>,
                        std::greater<>>;

bool CanRead(const int flags)
{
    return (flags & O_ACCMODE) != O_WRONLY;
}

bool CanWrite(const int flags)
{
    const int mode = flags & O_ACCMODE;
    return mode == O_WRONLY || mode == O_RDWR;
}

std::string NormalizePath(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

kio::MemoryBackend::IoFault TakeFault(std::deque<kio::MemoryBackend::IoFault>& faults)
{
    if (faults.empty())
    {
        return {};
    }

    auto fault = faults.front();
    faults.pop_front();
    return fault;
}

void DrainDueTimers(TimerQueue& timers, std::deque<kio::OperationState*>& ready, const kio::MemoryBackend::clock::time_point now)
{
    while (!timers.empty() && timers.top().due <= now)
    {
        ready.push_back(timers.top().op);
        timers.pop();
    }
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

template <typename Backend>
BasicIoContext<Backend>* BasicIoContext<Backend>::Current() noexcept
{
    if (tl_current_backend_tag != BackendTag<Backend>())
    {
        return nullptr;
    }
    return static_cast<BasicIoContext<Backend>*>(tl_current_context);
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

void UringBackend::Init(const unsigned entries)
{
    io_uring_params params{};
    params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    if (const int ret = io_uring_queue_init_params(entries, &ring_, &params); ret < 0)
    {
        throw std::system_error(-ret, std::system_category(), "io_uring_queue_init_params");
    }

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

    io_uring_prep_cancel(sqe, nullptr, IORING_ASYNC_CANCEL_ANY);
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

void MemoryBackend::Init(unsigned)
{
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0)
    {
        throw std::system_error(errno, std::system_category(), "eventfd");
    }
}

void MemoryBackend::Shutdown() noexcept
{
    if (wake_fd_ >= 0)
    {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
    wake_buffer_ = 0;
}

bool MemoryBackend::Notify() const noexcept
{
    if (wake_fd_ < 0)
    {
        return false;
    }
    constexpr uint64_t val = 1;
    return ::write(wake_fd_, &val, sizeof(val)) == sizeof(val);
}

void MemoryBackend::AddTimer(OperationState* op, const clock::time_point due)
{
    timers_.push({due, op});
}

void MemoryBackend::Complete(OperationState* op, const int32_t res)
{
    op->res = res;
    ready_.push_back(op);
}

Result<MemoryBackend::NativeFileHandle> MemoryBackend::OpenFile(const std::filesystem::path& path, const int flags,
                                                                mode_t)
{
    if (const auto fault = TakeFault(open_faults_); fault.error != 0)
    {
        return std::unexpected(make_error_code(fault.error));
    }

    const auto normalized = NormalizePath(path);
    auto it = files_.find(normalized);

    if ((flags & O_CREAT) != 0)
    {
        if ((flags & O_EXCL) != 0 && it != files_.end())
        {
            return std::unexpected(make_error_code(EEXIST));
        }

        if (it == files_.end())
        {
            it = files_.emplace(normalized, std::make_shared<FileState>()).first;
        }
    }
    else if (it == files_.end())
    {
        return std::unexpected(make_error_code(ENOENT));
    }

    auto file = it->second;
    if ((flags & O_TRUNC) != 0 && CanWrite(flags))
    {
        file->data.clear();
        file->fsynced = false;
    }

    const auto handle = next_handle_++;
    open_files_[handle] = OpenFileState{.file = std::move(file), .flags = flags};
    return handle;
}

Result<size_t> MemoryBackend::ReadFile(const NativeFileHandle handle, const std::span<std::byte> buffer,
                                       const uint64_t offset)
{
    const auto fault = TakeFault(read_faults_);
    if (fault.error != 0)
    {
        return std::unexpected(make_error_code(fault.error));
    }

    const auto it = open_files_.find(handle);
    if (it == open_files_.end() || !CanRead(it->second.flags))
    {
        return std::unexpected(make_error_code(EBADF));
    }

    const auto& data = it->second.file->data;
    if (offset >= data.size())
    {
        return size_t{0};
    }

    const auto available = data.size() - static_cast<size_t>(offset);
    auto n = std::min(buffer.size(), available);
    const auto max_bytes = fault.max_bytes != 0 ? fault.max_bytes : config_.default_max_read_bytes;
    if (max_bytes != 0)
    {
        n = std::min(n, max_bytes);
    }
    std::memcpy(buffer.data(), data.data() + static_cast<size_t>(offset), n);
    return n;
}

Result<size_t> MemoryBackend::WriteFile(const NativeFileHandle handle, const std::span<const std::byte> buffer,
                                        uint64_t offset)
{
    const auto fault = TakeFault(write_faults_);
    if (fault.error != 0)
    {
        return std::unexpected(make_error_code(fault.error));
    }

    const auto it = open_files_.find(handle);
    if (it == open_files_.end() || !CanWrite(it->second.flags))
    {
        return std::unexpected(make_error_code(EBADF));
    }

    auto& file = *it->second.file;
    if ((it->second.flags & O_APPEND) != 0)
    {
        offset = file.data.size();
    }

    auto bytes_to_write = buffer.size();
    const auto max_bytes = fault.max_bytes != 0 ? fault.max_bytes : config_.default_max_write_bytes;
    if (max_bytes != 0)
    {
        bytes_to_write = std::min(bytes_to_write, max_bytes);
    }

    const auto end = static_cast<size_t>(offset) + bytes_to_write;
    if (file.data.size() < end)
    {
        file.data.resize(end);
    }

    std::memcpy(file.data.data() + static_cast<size_t>(offset), buffer.data(), bytes_to_write);
    file.fsynced = false;
    return bytes_to_write;
}

Result<void> MemoryBackend::CloseFile(const NativeFileHandle handle)
{
    if (const auto fault = TakeFault(close_faults_); fault.error != 0)
    {
        return std::unexpected(make_error_code(fault.error));
    }

    if (!open_files_.erase(handle))
    {
        return std::unexpected(make_error_code(EBADF));
    }
    return {};
}

Result<void> MemoryBackend::FsyncFile(const NativeFileHandle handle)
{
    if (const auto fault = TakeFault(fsync_faults_); fault.error != 0)
    {
        return std::unexpected(make_error_code(fault.error));
    }

    const auto it = open_files_.find(handle);
    if (it == open_files_.end())
    {
        return std::unexpected(make_error_code(EBADF));
    }

    it->second.file->fsynced = true;
    return {};
}

int MemoryBackend::SubmitAndWait(unsigned)
{
    for (;;)
    {
        auto now = Now();
        DrainDueTimers(timers_, ready_, now);

        if (!ready_.empty())
        {
            return 0;
        }

        if (!timers_.empty())
        {
            const auto next_due = timers_.top().due;
            if (next_due <= now)
            {
                continue;
            }

            if (config_.time_mode == TimeMode::AutoAdvance)
            {
                AdvanceTo(next_due);
                continue;
            }
        }

        pollfd pfd{
            .fd = wake_fd_,
            .events = POLLIN,
            .revents = 0,
        };
        int ret = 0;
        do
        {
            ret = ::poll(&pfd, 1, -1);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0)
        {
            return -errno;
        }

        if (ret == 0)
        {
            continue;
        }

        if ((pfd.revents & POLLIN) != 0)
        {
            while (::read(wake_fd_, &wake_buffer_, sizeof(wake_buffer_)) == sizeof(wake_buffer_))
            {
            }
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }
    }
}

void MemoryBackend::CancelAllPending()
{
    while (!timers_.empty())
    {
        ready_.push_back(timers_.top().op);
        timers_.pop();
    }
    (void)Notify();
}

template <typename Backend>
BasicIoContext<Backend>::BasicIoContext(const unsigned entries)
{
    ready_.reserve(entries);
    backend_.Init(entries);

    ready_latch_.count_down();
}

template <typename Backend>
BasicIoContext<Backend>::BasicIoContext(Backend backend, const unsigned entries) : backend_(std::move(backend))
{
    ready_.reserve(entries);
    backend_.Init(entries);

    ready_latch_.count_down();
}

template <typename Backend>
BasicIoContext<Backend>::~BasicIoContext() noexcept
{
    CancelAllPending();
    backend_.Shutdown();
}

template <typename Backend>
bool BasicIoContext<Backend>::Notify() const noexcept
{
    return backend_.Notify();
}

template <typename Backend>
void BasicIoContext<Backend>::Track(OperationState* op)
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

template <typename Backend>
void BasicIoContext<Backend>::Untrack(OperationState* op)
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

template <typename Backend>
void BasicIoContext<Backend>::CancelAllPending(const OpCancelReason reason)
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

template <typename Backend>
Result<> BasicIoContext<Backend>::RegisterFiles(const std::span<const int> fds)
{
    AssertOwnerThread();
    return backend_.RegisterFiles(fds);
}

// -----------------------------------------------------------------------------
// Cross-Thread Scheduling Logic
// -----------------------------------------------------------------------------

template <typename Backend>
bool BasicIoContext<Backend>::TryMsgRing(const BasicIoContext& target, OperationState* op)
{
    return backend_.TryMsgRing(target.GetBackend(), op);
}

template <typename Backend>
void BasicIoContext<Backend>::SubmitExternal(OperationState* op)
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

template <typename Backend>
void BasicIoContext<Backend>::DrainExternal(std::vector<std::coroutine_handle<>>& out)
{
    // Clear hint
    ext_hint_.store(false, std::memory_order_release);

    // Steal list
    OperationState* head = ext_submission_head_.exchange(nullptr, std::memory_order_acquire);

    if (head == nullptr)
        return;

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

template <typename Backend>
void BasicIoContext<Backend>::DrainExternalWithoutResume()
{
    ext_hint_.store(false, std::memory_order_release);
    OperationState* head = ext_submission_head_.exchange(nullptr, std::memory_order_acquire);

    if (head == nullptr)
        return;

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

template <typename Backend>
void BasicIoContext<Backend>::DrainWithoutResume()
{
    while (pending_head_ != nullptr)
    {
        const bool got_completion = backend_.DrainWithoutResume([&](const uint64_t user_data)
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

template <typename Backend>
void BasicIoContext<Backend>::SubmitWakeRead()
{
    backend_.SubmitWakeRead();
}

template <typename Backend>
void BasicIoContext<Backend>::Step()
{
    const int ret = backend_.SubmitAndWait(1);
    if (ret < 0)
        return;

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
            h.resume();
    }
}

template <typename Backend>
std::pair<unsigned, bool> BasicIoContext<Backend>::ProcessReadyCompletions()
{
    return backend_.ProcessReadyCompletions([this](OperationState* op, const int32_t res)
    {
        Untrack(op);
        op->res = res;
        ready_.push_back(op->handle);

#if AIO_STATS
        AIO_STATS_INC(stats_, ops_completed);
        if (res < 0)
            AIO_STATS_INC(stats_, ops_errors);
#endif
    });
}

SignalSet::SignalSet(const std::initializer_list<int> sigs) : fd_(eventfd(0, EFD_CLOEXEC))
{
    if (fd_ < 0)
        throw std::system_error(errno, std::system_category(), "eventfd");
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
        ::close(fd_);
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
        return std::unexpected(make_error_code(res));
    return static_cast<int>(signo);
}

template class BasicIoContext<UringBackend>;
template class BasicIoContext<MemoryBackend>;
}  // namespace kio
