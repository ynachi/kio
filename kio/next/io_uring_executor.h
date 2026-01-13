//
// io_uring_executor.h - Production Version
// Features:
// - Fixed critical hang bug (scheduleOn failure handling)
// - Explicit submission context (.on_context())
// - Explicit resume policy (.resume_on())
//

#ifndef KIO_CORE_URING_EXECUTOR_H
#define KIO_CORE_URING_EXECUTOR_H

#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <system_error>
#include <cstdint>

#include <liburing.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include <async_simple/Executor.h>
#include <ylt/util/concurrentqueue.h>

namespace kio::next::v1
{

struct IoUringExecutorConfig
{
    size_t num_threads = std::thread::hardware_concurrency();
    uint32_t io_uring_entries = 32768;
    uint32_t io_uring_flags = 0;
    bool pin_threads = false;
};

class IoUringExecutor : public async_simple::Executor
{
public:
    using Func = std::function<void()>;
    using Task = Func;

    explicit IoUringExecutor(const IoUringExecutorConfig& config = IoUringExecutorConfig{});
    ~IoUringExecutor() override;

    IoUringExecutor(const IoUringExecutor&) = delete;
    IoUringExecutor& operator=(const IoUringExecutor&) = delete;
    IoUringExecutor(IoUringExecutor&&) = delete;
    IoUringExecutor& operator=(IoUringExecutor&&) = delete;

    // async_simple::Executor interface
    bool schedule(Func func) override;
    [[nodiscard]] bool currentThreadInExecutor() const override;
    [[nodiscard]] size_t currentContextId() const override;
    Context checkout() override;
    bool checkin(Func func, Context ctx, async_simple::ScheduleOptions opts) override;

    // Extended interface
    bool scheduleOn(size_t context_id, Func func);
    bool scheduleLocal(Func func);
    size_t pickContextId();

    void stop();
    void join();

    [[nodiscard]] size_t numThreads() const { return contexts_.size(); }
    [[nodiscard]] io_uring* getRing(size_t context_id) const { return &contexts_[context_id]->ring; }

private:
    struct PerThreadContext
    {
        io_uring ring{};
        ylt::detail::moodycamel::ConcurrentQueue<Task> task_queue;
        int eventfd;
        std::thread::id thread_id;
        size_t context_id;
        std::atomic<bool> running{true};

        explicit PerThreadContext(size_t id) : eventfd(-1), context_id(id) {}

        ~PerThreadContext()
        {
            if (eventfd >= 0)
            {
                close(eventfd);
            }
        }
    };

    static thread_local PerThreadContext* current_context_;

    std::vector<std::unique_ptr<PerThreadContext>> contexts_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopped_{false};
    std::atomic<size_t> next_context_{0};

    void runEventLoop(PerThreadContext* ctx);
    void processLocalQueue(PerThreadContext* ctx);
    void wakeThread(PerThreadContext& ctx);
    PerThreadContext& selectContext();
    void pinThreadToCpu(size_t thread_id, size_t cpu_id);
    static void handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe);
};

// Base interface for I/O operations
struct IoOp
{
    virtual void complete(int res) = 0;
    virtual ~IoOp() = default;
};

// Resume policy for I/O operations
enum class ResumeMode : uint8_t
{
    InlineOnSubmitCtx,  // Resume inline on submit context (fastest, current behavior)
    ViaCheckin          // Resume via executor->checkin() (hop back to original context)
};

// Production-grade awaiter with explicit control
template <typename ResultType, typename PrepareFunc>
class IoUringAwaiter : public IoOp
{
public:
    using result_type = ResultType;

    IoUringAwaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
        : executor_(executor), prepare_func_(std::move(prepare_func))
    {
    }

    // Fluent API: Explicitly set submission context
    IoUringAwaiter&& on_context(size_t ctx) &&
    {
        forced_ctx_ = true;
        context_id_ = ctx;
        return std::move(*this);
    }

    // Fluent API: Explicitly set resume context
    IoUringAwaiter&& resume_on(async_simple::Executor::Context home,
                               async_simple::ScheduleOptions opts = {}) &&
    {
        resume_mode_ = ResumeMode::ViaCheckin;
        home_ctx_ = home;
        opts_ = opts;
        return std::move(*this);
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        continuation_ = h;

        // Pick submit context (explicit or automatic)
        if (!forced_ctx_)
        {
            context_id_ = executor_->currentThreadInExecutor()
                ? executor_->currentContextId()
                : executor_->pickContextId();
        }

        // Capture "home" context if using checkin-based resume but no explicit context provided
        if (resume_mode_ == ResumeMode::ViaCheckin && home_ctx_ == IoUringExecutor::NULLCTX)
        {
            home_ctx_ = executor_->checkout();
        }

        // Fast path: already on target context
        if (executor_->currentThreadInExecutor() &&
            executor_->currentContextId() == context_id_)
        {
            return submit_sqe_or_resume_error();
        }

        // Cross-thread submission
        // CRITICAL FIX: Handle scheduleOn failure to prevent coroutine hang
        bool ok = executor_->scheduleOn(context_id_, [this]() {
            if (!this->submit_sqe())
            {
                this->result_ = -EBUSY;
                this->complete(this->result_);
            }
        });

        if (!ok)
        {
            // CRITICAL: Don't suspend if scheduleOn failed!
            // Returning true would hang the coroutine forever.
            result_ = -ECANCELED;
            return false;  // Resume immediately, await_resume will throw
        }

        return true;  // Safe to suspend
    }

    ResultType await_resume()
    {
        if (result_ < 0)
        {
            errno = -result_;
            throw std::system_error(errno, std::system_category(), "io_uring operation failed");
        }
        return static_cast<ResultType>(result_);
    }

    void complete(int res) override
    {
        result_ = res;
        if (!continuation_) return;

        if (resume_mode_ == ResumeMode::InlineOnSubmitCtx)
        {
            // Fast path: resume inline (current behavior)
            continuation_.resume();
            return;
        }

        // Resume via async_simple checkin (hop back to original context)
        auto h = continuation_;
        continuation_ = {};  // Clear before checkin to prevent double-resume
        executor_->checkin([h]() mutable { h.resume(); }, home_ctx_, opts_);
    }

private:
    bool submit_sqe_or_resume_error() noexcept
    {
        if (submit_sqe()) return true;

        // Submit failed, don't suspend
        result_ = -EBUSY;
        return false;  // await_resume will throw
    }

    bool submit_sqe() noexcept
    {
        io_uring* ring = executor_->getRing(context_id_);
        io_uring_sqe* sqe = io_uring_get_sqe(ring);

        if (!sqe)
        {
            // Optional improvement: try submitting pending ops and retry once
            // io_uring_submit(ring);
            // sqe = io_uring_get_sqe(ring);
            // if (!sqe) return false;
            return false;
        }

        prepare_func_(sqe);
        io_uring_sqe_set_data(sqe, static_cast<IoOp*>(this));
        return true;
    }

    IoUringExecutor* executor_;
    size_t context_id_{0};
    bool forced_ctx_{false};

    PrepareFunc prepare_func_;
    std::coroutine_handle<> continuation_;
    int result_{0};

    // Resumption control
    ResumeMode resume_mode_{ResumeMode::InlineOnSubmitCtx};
    async_simple::Executor::Context home_ctx_{IoUringExecutor::NULLCTX};
    async_simple::ScheduleOptions opts_{};
};

template <typename ResultType, typename PrepareFunc>
auto make_io_awaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
{
    return IoUringAwaiter<ResultType, std::decay_t<PrepareFunc>>(
        executor, std::forward<PrepareFunc>(prepare_func));
}

}  // namespace kio::next::v1

#endif  // KIO_CORE_URING_EXECUTOR_H