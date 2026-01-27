#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <latch>
#include <optional>
#include <stop_token>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "io_context.hpp"

namespace aio
{

class Worker
{
public:
    explicit Worker(const int id = next_id_++, std::stop_token external_st = {})
        : id_(id), stop_token_(std::move(external_st))
    {
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    Worker(Worker&& other) noexcept
        : thread_(std::move(other.thread_)),
          wake_fd_(std::exchange(other.wake_fd_, -1)),
          id_(std::exchange(other.id_, -1)),
          tid_(std::exchange(other.tid_, 0)),
          stop_token_(std::move(other.stop_token_))
    {
    }

    Worker& operator=(Worker&& other) noexcept
    {
        if (this != &other)
        {
            Join();
            thread_ = std::move(other.thread_);
            wake_fd_ = std::exchange(other.wake_fd_, -1);
            id_ = std::exchange(other.id_, -1);
            tid_ = std::exchange(other.tid_, 0);
            stop_token_ = std::move(other.stop_token_);
        }
        return *this;
    }

    ~Worker() = default;

    /**
     * Low-level: spawns a thread, creates IoContext, runs user function.
     * User function receives IoContext& and is responsible for calling Run()/RunUntilDone().
     */
    template <typename F>
    void Start(F&& func, int cpu_id = -1)
    {
        std::latch ready{1};
        std::atomic fd_out{-1};
        std::atomic<uint32_t> tid_out{0};

        auto ext_st = stop_token_;

        thread_ = std::jthread(
            [&ready, &fd_out, &tid_out, func = std::forward<F>(func), cpu_id, ext_st](std::stop_token st) mutable
            {
                if (cpu_id >= 0)
                {
                    PinToCpu(cpu_id);
                }

                const auto tid = static_cast<uint32_t>(::syscall(SYS_gettid));
                IoContext ctx(1024);

                auto stop_action = [&ctx]
                {
                    ctx.Stop();
                    (void)ctx.Notify();
                };

                std::stop_callback cb_internal(st, stop_action);

                if (ext_st.stop_possible())
                {
                    std::optional<std::stop_callback<decltype(stop_action)>> cb_external;
                    cb_external.emplace(ext_st, stop_action);
                }

                fd_out.store(ctx.WakeFd(), std::memory_order_relaxed);
                tid_out.store(tid, std::memory_order_relaxed);
                ready.count_down();

                func(ctx);
            });

        ready.wait();
        wake_fd_ = fd_out.load(std::memory_order_relaxed);
        tid_ = tid_out.load(std::memory_order_relaxed);
    }

    /**
     * Convenience: runs ctx.Run(tick) in a loop.
     */
    template <typename Tick>
    void RunLoop(Tick&& tick, int cpu_id = -1)
    {
        Start(
            [tick = std::forward<Tick>(tick)](IoContext& ctx) mutable
            {
                ctx.Run(tick);
                ctx.CancelAllPending();
            },
            cpu_id);
    }

    /**
     * Convenience: runs ctx.Run() with no tick.
     */
    void RunLoop(const int cpu_id = -1)
    {
        Start(
            [](IoContext& ctx)
            {
                ctx.Run();
                ctx.CancelAllPending();
            },
            cpu_id);
    }

    /**
     * Convenience: runs a single task to completion.
     * TaskFactory: (IoContext&) -> Task<T>
     */
    template <typename TaskFactory>
    void RunTask(TaskFactory&& factory, int cpu_id = -1)
    {
        Start(
            [factory = std::forward<TaskFactory>(factory)](IoContext& ctx) mutable
            {
                auto task = factory(ctx);
                ctx.RunUntilDone(std::move(task));
                ctx.CancelAllPending();
            },
            cpu_id);
    }

    void RequestStop() { thread_.request_stop(); }

    void Join()
    {
        if (thread_.joinable())
            thread_.join();
    }

    [[nodiscard]] bool Joinable() const { return thread_.joinable(); }

    bool Notify() const
    {
        if (wake_fd_ < 0)
            return false;
        constexpr uint64_t val = 1;
        return ::write(wake_fd_, &val, sizeof(val)) == sizeof(val);
    }

    [[nodiscard]] int Id() const { return id_; }
    [[nodiscard]] uint32_t ThreadId() const { return tid_; }
    [[nodiscard]] int WakeFd() const { return wake_fd_; }
    [[nodiscard]] std::stop_token ExternalStopToken() const { return stop_token_; }

private:
    static void PinToCpu(int cpu_id)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id % static_cast<int>(std::thread::hardware_concurrency()), &cpuset);

        if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
        {
            ALOG_ERROR("Warning: Failed to pin to CPU {}: {}", cpu_id, std::strerror(rc));
        }
    }

    static inline std::atomic<int> next_id_{0};

    std::jthread thread_;
    int wake_fd_ = -1;
    int id_;
    uint32_t tid_ = 0;
    std::stop_token stop_token_;
};

}  // namespace aio