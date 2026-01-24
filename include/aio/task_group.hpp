#pragma once

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "aio/events.hpp"
#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/task.hpp"

namespace aio
{

template <typename T = void>
class TaskGroup
{
public:
    explicit TaskGroup(IoContext* ctx = nullptr, size_t reserve_capacity = 64) : ctx_(ctx)
    {
        tasks_.reserve(reserve_capacity);
        if (ctx_ != nullptr)
        {
            completion_event_ = std::make_unique<Event>(ctx_);
        }
    }

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    TaskGroup(TaskGroup&& other) noexcept
        : ctx_(other.ctx_),
          tasks_(std::move(other.tasks_)),
          sweep_interval_(other.sweep_interval_),
          spawn_count_(other.spawn_count_),
          active_count_(other.active_count_),
          waiting_(other.waiting_),
          completion_event_(std::move(other.completion_event_))
    {
        other.ctx_ = nullptr;
        other.active_count_ = 0;
        other.waiting_ = false;
    }

    TaskGroup& operator=(TaskGroup&& other) noexcept
    {
        if (this != &other)
        {
            ctx_ = other.ctx_;
            tasks_ = std::move(other.tasks_);
            sweep_interval_ = other.sweep_interval_;
            spawn_count_ = other.spawn_count_;
            active_count_ = other.active_count_;
            completion_event_ = std::move(other.completion_event_);
            waiting_ = other.waiting_;

            other.ctx_ = nullptr;
            other.active_count_ = 0;
            other.waiting_ = false;
        }
        return *this;
    }

    ~TaskGroup() noexcept = default;

    // -------------------------------------------------------------------------
    // Task Spawning
    // -------------------------------------------------------------------------

    void Spawn(Task<T>&& t)
    {
        // Wrap the task to intercept completion
        auto wrapped = [](Task<T> original, TaskGroup* group) -> Task<T>
        {
            // RAII guard to ensure we decrement even on exception
            struct CompletionGuard
            {
                TaskGroup* g;
                ~CompletionGuard() { g->OnTaskDone(); }
            } guard{group};

            if constexpr (std::is_void_v<T>)
            {
                co_await original;
            }
            else
            {
                co_return co_await original;
            }
        }(std::move(t), this);

        wrapped.Start();
        tasks_.push_back(std::move(wrapped));

        ++spawn_count_;
        ++active_count_;

        if ((spawn_count_ & (sweep_interval_ - 1)) == 0)
        {
            Sweep();
        }
    }

    template <typename... Tasks>
    void SpawnAll(Tasks&&... tasks)
    {
        (Spawn(std::forward<Tasks>(tasks)), ...);
    }

    // -------------------------------------------------------------------------
    // Lifetime Management
    // -------------------------------------------------------------------------

    size_t Sweep()
    {
        const size_t before = tasks_.size();
        std::erase_if(tasks_, [](Task<T>& t) { return t.Done(); });
        return before - tasks_.size();
    }

    void SetSweepInterval(size_t interval)
    {
        size_t pow2 = 1;
        while (pow2 < interval)
            pow2 <<= 1;
        sweep_interval_ = pow2;
    }

    // -------------------------------------------------------------------------
    // Waiting / Joining
    // -------------------------------------------------------------------------

    Task<> JoinAll(IoContext& ctx)
    {
        // Note: ctx param kept for API compatibility.
        // Ideally we rely on the internal ctx_ passed in constructor.

        if (active_count_ == 0)
            co_return;

        // Fast path: if event exists, use it for zero-latency wake
        if (completion_event_)
        {
            waiting_ = true;
            while (active_count_ > 0)
            {
                co_await completion_event_->Wait();
            }
            waiting_ = false;
        }
        else
        {
            // Fallback to polling if no context was provided at construction
            while (active_count_ > 0)
            {
                Sweep();
                co_await AsyncSleep(ctx, std::chrono::milliseconds(10));
            }
        }
        Sweep();
    }

    Task<bool> JoinAllTimeout(IoContext& ctx, std::chrono::milliseconds timeout)
    {
        if (active_count_ == 0)
            co_return true;

        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (completion_event_)
        {
            waiting_ = true;

            while (active_count_ > 0)
            {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    waiting_ = false;
                    co_return false;
                }

                // Wait with the remaining time
                auto result = co_await completion_event_->Wait().WithTimeout(deadline - now);
                if (!result && result.error() == std::errc::timed_out)
                {
                    waiting_ = false;
                    co_return false;
                }
            }
            waiting_ = false;
        }
        else
        {
            // Fallback poll
            while (active_count_ > 0)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    co_return false;
                Sweep();
                co_await AsyncSleep(ctx, std::chrono::milliseconds(10));
            }
        }

        Sweep();
        co_return true;
    }

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    size_t Size() const { return tasks_.size(); }
    size_t ActiveCount() const { return active_count_; }
    bool AllDone() const { return active_count_ == 0; }
    bool Empty() const { return tasks_.empty(); }
    uint64_t TotalSpawned() const { return spawn_count_; }
    std::vector<Task<T>>& Tasks() { return tasks_; }
    const std::vector<Task<T>>& Tasks() const { return tasks_; }

    void OnTaskDone()
    {
        if (--active_count_ == 0)
        {
            if (waiting_ && completion_event_)
            {
                completion_event_->Signal();
            }
        }
    }

private:
    IoContext* ctx_;
    std::vector<Task<T>> tasks_;
    size_t sweep_interval_ = 1024;
    uint64_t spawn_count_ = 0;

    size_t active_count_ = 0;
    bool waiting_ = false;
    std::unique_ptr<Event> completion_event_;
};

using void_task_group = TaskGroup<>;

}  // namespace aio