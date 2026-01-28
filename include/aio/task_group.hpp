#pragma once

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

#include "aio/core.hpp"

namespace aio
{

/**
 * task_group.hpp - Structured concurrency for tasks
 *
 * Purpose:
 * Manages the lifetime of a collection of Tasks. It allows you to spawn multiple
 * coroutines concurrently and wait for them all to complete. This is the primary
 * mechanism for parallel/concurrent execution in this library.
 *
 * Key Features:
 * - Ownership: Takes ownership of spawned Tasks, preventing destruction while running.
 * - Synchronization: Provides JoinAll() to await completion of all tasks in the group.
 * - Memory Management: Automatically sweeps completed tasks to free memory.
 *
 * Thread Safety:
 * - Not thread-safe. A TaskGroup should generally be used from a single thread
 * (typically the thread running the IoContext loop).
 *
 * Usage:
 * TaskGroup<> group(&ctx);
 * group.Spawn(MyTask(ctx));
 * group.Spawn(MyOtherTask(ctx));
 * co_await group.JoinAll(ctx);
 */
template <typename T = void>
class TaskGroup
{
public:
    explicit TaskGroup(size_t reserve_capacity = 64)
    {
        tasks_.reserve(reserve_capacity);
        completion_notifier_ = std::make_unique<Notifier>();
    }

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    TaskGroup(TaskGroup&&) = default;
    TaskGroup& operator=(TaskGroup&&) = default;

    ~TaskGroup() noexcept = default;

    void Spawn(Task<T>&& t)
    {
        auto wrapped = [](Task<T> original, TaskGroup* group) -> Task<T>
        {
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

    Task<> JoinAll(IoContext& ctx)
    {
        if (active_count_ == 0)
            co_return;

        waiting_ = true;
        while (active_count_ > 0)
        {
            co_await completion_notifier_->Wait(ctx);
        }
        waiting_ = false;
        Sweep();
    }

    Task<bool> JoinAllTimeout(IoContext& ctx, std::chrono::milliseconds timeout)
    {
        if (active_count_ == 0)
            co_return true;

        const auto deadline = std::chrono::steady_clock::now() + timeout;

        waiting_ = true;
        while (active_count_ > 0)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                waiting_ = false;
                co_return false;
            }

            auto result = co_await completion_notifier_->Wait(ctx).WithTimeout(deadline - now);
            if (!result && result.error() == std::errc::timed_out)
            {
                waiting_ = false;
                co_return false;
            }
        }
        waiting_ = false;
        Sweep();
        co_return true;
    }

    size_t Size() const { return tasks_.size(); }
    size_t ActiveCount() const { return active_count_; }
    bool AllDone() const { return active_count_ == 0; }
    bool Empty() const { return tasks_.empty(); }
    uint64_t TotalSpawned() const { return spawn_count_; }
    std::vector<Task<T>>& Tasks() { return tasks_; }
    const std::vector<Task<T>>& Tasks() const { return tasks_; }

    void OnTaskDone()
    {
        if (--active_count_ == 0 || waiting_)
        {
            completion_notifier_->Signal();
        }
    }

private:
    std::vector<Task<T>> tasks_;
    std::unique_ptr<Notifier> completion_notifier_;
    size_t sweep_interval_ = 1024;
    uint64_t spawn_count_ = 0;
    size_t active_count_ = 0;
    bool waiting_ = false;
};

using void_task_group = TaskGroup<>;

}  // namespace aio