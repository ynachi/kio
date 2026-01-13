//
// io_uring_executor.h
// Fixed: Smart Waking & Thread-Safe IO
//

#ifndef KIO_CORE_URING_EXECUTOR_V1_H
#define KIO_CORE_URING_EXECUTOR_V1_H

#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <system_error>

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

        // Smart Waking Flag: "true" means thread is blocked on read(eventfd)
        std::atomic<bool> sleeping{false};

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
    bool processLocalQueue(PerThreadContext* ctx);
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

// Zero-allocation awaiter for io_uring operations
template <typename ResultType, typename PrepareFunc>
class IoUringAwaiter : public IoOp
{
public:
    using result_type = ResultType;

    IoUringAwaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
        : executor_(executor), prepare_func_(std::move(prepare_func))
    {
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        continuation_ = h;

        // 1. Determine Target Context
        // If sticky, stay here. If external, pick one.
        context_id_ = executor_->currentThreadInExecutor()
            ? executor_->currentContextId()
            : executor_->pickContextId();

        // 2. Thread Safety Check
        // Only touch the ring if we are ON the correct thread.
        if (executor_->currentThreadInExecutor() &&
            executor_->currentContextId() == context_id_)
        {
            return submit_sqe();
        }
        else
        {
            // Cross-thread submission: Schedule as task.
            executor_->scheduleOn(context_id_, [this]() {
                if (!this->submit_sqe()) {
                    this->result_ = -EBUSY;
                    if (this->continuation_) this->continuation_.resume();
                }
            });
            return true;
        }
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
        if (continuation_)
        {
            continuation_.resume();
        }
    }

private:
    bool submit_sqe() {
        io_uring* ring = executor_->getRing(context_id_);
        io_uring_sqe* sqe = io_uring_get_sqe(ring);

        if (!sqe)
        {
            result_ = -EBUSY;
            return false;
        }

        prepare_func_(sqe);
        io_uring_sqe_set_data(sqe, static_cast<IoOp*>(this));
        return true;
    }

    IoUringExecutor* executor_;
    size_t context_id_{0};
    PrepareFunc prepare_func_;
    std::coroutine_handle<> continuation_;
    int result_ = 0;
};

template <typename ResultType, typename PrepareFunc>
auto make_io_awaiter(IoUringExecutor* executor, PrepareFunc&& prepare_func)
{
    return IoUringAwaiter<ResultType, std::decay_t<PrepareFunc>>(
        executor, std::forward<PrepareFunc>(prepare_func));
}

}  // namespace kio::next::v1

#endif  // KIO_CORE_URING_EXECUTOR_V1_H