#pragma once

#include <algorithm>
#include <chrono>
#include <vector>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/task.hpp"

namespace aio {

template<typename T = void>
class TaskGroup {
public:
    explicit TaskGroup(IoContext* ctx = nullptr, size_t reserve_capacity = 64)
        : ctx_(ctx) {
        tasks_.reserve(reserve_capacity);
    }
    
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    
    TaskGroup(TaskGroup&& other) noexcept
        : ctx_(other.ctx_)
        , tasks_(std::move(other.tasks_))
        , sweep_interval_(other.sweep_interval_)
        , spawn_count_(other.spawn_count_) {
        other.ctx_ = nullptr;
    }
    
    TaskGroup& operator=(TaskGroup&& other) noexcept {
        if (this != &other) {
            ctx_ = other.ctx_;
            tasks_ = std::move(other.tasks_);
            sweep_interval_ = other.sweep_interval_;
            spawn_count_ = other.spawn_count_;
            other.ctx_ = nullptr;
        }
        return *this;
    }
    
    ~TaskGroup() noexcept = default;
    
    // -------------------------------------------------------------------------
    // Task Spawning
    // -------------------------------------------------------------------------
    
    void Spawn(Task<T>&& t) {
        t.Start();
        tasks_.push_back(std::move(t));
        ++spawn_count_;
        
        if ((spawn_count_ & (sweep_interval_ - 1)) == 0) {
            Sweep();
        }
    }
    
    template<typename... Tasks>
    void SpawnAll(Tasks&&... tasks) {
        (Spawn(std::forward<Tasks>(tasks)), ...);
    }
    
    // -------------------------------------------------------------------------
    // Lifetime Management
    // -------------------------------------------------------------------------
    
    size_t Sweep() {
        const size_t before = tasks_.size();
        std::erase_if(tasks_, [](Task<T>& t) { return t.Done(); });
        return before - tasks_.size();
    }
    
    void SetSweepInterval(size_t interval) {
        size_t pow2 = 1;
        while (pow2 < interval) pow2 <<= 1;
        sweep_interval_ = pow2;
    }
    
    // -------------------------------------------------------------------------
    // Waiting / Joining
    // -------------------------------------------------------------------------
    
    Task<> JoinAll(IoContext& ctx, std::chrono::milliseconds poll = std::chrono::milliseconds(10));
    
    Task<bool> JoinAllTimeout(IoContext& ctx, std::chrono::milliseconds timeout);
    
    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------
    
    size_t Size() const { return tasks_.size(); }
    
    size_t ActiveCount() const {
        return std::count_if(tasks_.begin(), tasks_.end(),
                            [](const Task<T>& t) { return !t.Done(); });
    }
    
    bool AllDone() const {
        return std::all_of(tasks_.begin(), tasks_.end(),
                          [](const Task<T>& t) { return t.Done(); });
    }
    
    bool Empty() const { return tasks_.empty(); }
    
    uint64_t TotalSpawned() const { return spawn_count_; }
    
    std::vector<Task<T>>& Tasks() { return tasks_; }
    const std::vector<Task<T>>& Tasks() const { return tasks_; }

private:
    IoContext* ctx_;
    std::vector<Task<T>> tasks_;
    size_t sweep_interval_ = 1024;
    uint64_t spawn_count_ = 0;
};

// Out-of-line definitions (need full io_context definition)
template<typename T>
Task<> TaskGroup<T>::JoinAll(IoContext& ctx, std::chrono::milliseconds poll) {
    // TODO: a bit inefficient but ok for now. Blocking_pool is for occasional cpu intensive
    // tasks
    while (!AllDone()) {
        Sweep();
        co_await AsyncSleep(ctx, poll);
    }
    Sweep();
}

template<typename T>
Task<bool> TaskGroup<T>::JoinAllTimeout(IoContext& ctx, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    
    while (!AllDone()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        Sweep();
        co_await AsyncSleep(ctx, std::chrono::milliseconds(10));
    }
    
    Sweep();
    co_return true;
}

using void_task_group = TaskGroup<>;

} // namespace aio
