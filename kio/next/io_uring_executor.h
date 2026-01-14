//
// io_uring_executor.h - Production Version
// Features:
// - Fixed Race: eventfd initialized in Context constructor
// - Fixed Race: Latch lifetime extension
// - Robust Event Loop
//

#ifndef KIO_CORE_URING_EXECUTOR_H
#define KIO_CORE_URING_EXECUTOR_H

#include <atomic>
#include <coroutine>
#include <functional>
#include <latch>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>  // For O_NONBLOCK
#include <liburing.h>
#include <poll.h>
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
    size_t local_batch_size = 16;
    bool pin_threads = false;
};

class IoUringExecutor : public async_simple::Executor
{
public:
    using Func = std::function<void()>;
    using Task = Func;

    explicit IoUringExecutor(const IoUringExecutorConfig& config = IoUringExecutorConfig{});
    ~IoUringExecutor() override;

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
    [[nodiscard]] io_uring* getRing(const size_t context_id) const { return &contexts_[context_id]->ring; }
    [[nodiscard]] std::stop_token getStopToken() const noexcept { return stop_source_.get_token(); }

private:
    struct PerThreadContext
    {
        io_uring ring{};
        ylt::detail::moodycamel::ConcurrentQueue<Task> task_queue;
        int eventfd{-1};
        std::thread::id thread_id;
        size_t context_id;
        std::atomic<bool> running{true};
        std::atomic<bool> needs_wakeup{false};
        std::stop_token stop_token;

        explicit PerThreadContext(const size_t id) : context_id(id)
        {
            // Create eventfd HERE (Main Thread) before worker starts.
            // This guarantees 'eventfd' is visible and valid for any thread trying to wake us.
            eventfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (eventfd < 0)
            {
                throw std::system_error(errno, std::system_category(), "eventfd creation failed");
            }
        }

        bool setStopped()
        {
            bool expected = true;
            return running.compare_exchange_strong(expected, false);
        }

        ~PerThreadContext()
        {
            setStopped();
            if (eventfd >= 0)
            {
                close(eventfd);
            }
        }
    };

    static thread_local PerThreadContext* current_context_;

    std::vector<std::unique_ptr<PerThreadContext>> contexts_;
    std::vector<std::jthread> threads_;
    std::stop_source stop_source_;
    std::atomic<bool> executor_stopped_{false};
    std::latch stop_latch_;
    std::atomic<size_t> next_context_{0};
    IoUringExecutorConfig executor_config_;

    void runEventLoop(PerThreadContext* ctx);
    bool processLocalQueue(PerThreadContext* ctx, size_t batch = 16);
    void wakeThread(PerThreadContext& ctx);
    PerThreadContext& selectContext();
    void pinThreadToCpu(size_t thread_id, size_t cpu_id);
    static void handleCompletion(PerThreadContext* ctx, io_uring_cqe* cqe);
};

struct IoOp
{
    virtual void complete(int res) = 0;
    virtual ~IoOp() = default;
};

enum class ResumeMode : uint8_t
{
    InlineOnSubmitCtx,
    ViaCheckin
};

template <typename ResultType, typename PrepareFunc>
class IoUringAwaiter : public IoOp
{
public:
    using result_type = ResultType;

    IoUringAwaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
        : executor_(executor), prepare_func_(std::move(prepare_func))
    {
    }

    IoUringAwaiter&& on_context(size_t ctx) &&
    {
        forced_ctx_ = true;
        context_id_ = ctx;
        return std::move(*this);
    }

    IoUringAwaiter&& resume_on(async_simple::Executor::Context home, async_simple::ScheduleOptions opts = {}) &&
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

        if (!forced_ctx_)
        {
            context_id_ =
                executor_->currentThreadInExecutor() ? executor_->currentContextId() : executor_->pickContextId();
        }

        if (resume_mode_ == ResumeMode::ViaCheckin && home_ctx_ == IoUringExecutor::NULLCTX)
        {
            home_ctx_ = executor_->checkout();
        }

        if (executor_->currentThreadInExecutor() && executor_->currentContextId() == context_id_)
        {
            return submit_sqe_or_resume_error();
        }

        const bool ok = executor_->scheduleOn(context_id_,
                                              [this]()
                                              {
                                                  if (!this->submit_sqe())
                                                  {
                                                      this->result_ = -EBUSY;
                                                      this->complete(this->result_);
                                                  }
                                              });

        if (!ok)
        {
            result_ = -ECANCELED;
            return false;
        }

        return true;
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

    void complete(const int res) override
    {
        result_ = res;
        if (!continuation_)
            return;

        if (resume_mode_ == ResumeMode::InlineOnSubmitCtx)
        {
            continuation_.resume();
            return;
        }

        auto h = continuation_;
        continuation_ = {};
        executor_->checkin([h]() mutable { h.resume(); }, home_ctx_, opts_);
    }

private:
    bool submit_sqe_or_resume_error() noexcept
    {
        if (submit_sqe())
            return true;
        result_ = -EBUSY;
        return false;
    }

    bool submit_sqe() noexcept
    {
        io_uring* ring = executor_->getRing(context_id_);
        io_uring_sqe* sqe = io_uring_get_sqe(ring);

        if (!sqe)
        {
            if (const int submitted = io_uring_submit(ring); submitted < 0)
                return false;
            sqe = io_uring_get_sqe(ring);
            if (!sqe)
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

    ResumeMode resume_mode_{ResumeMode::InlineOnSubmitCtx};
    async_simple::Executor::Context home_ctx_{IoUringExecutor::NULLCTX};
    async_simple::ScheduleOptions opts_{};
};

template <typename ResultType, typename PrepareFunc>
auto make_io_awaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
{
    return IoUringAwaiter<ResultType, std::decay_t<PrepareFunc>>(executor, std::forward<PrepareFunc>(prepare_func));
}

}  // namespace kio::next::v1

#endif  // KIO_CORE_URING_EXECUTOR_H