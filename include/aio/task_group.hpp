#pragma once

#include <algorithm>
#include <chrono>
#include <vector>

#include "aio/io_context.hpp"
#include "aio/task.hpp"
#include "aio/io.hpp"

namespace aio {

template<typename T = void>
class task_group {
public:
    explicit task_group(io_context* ctx = nullptr, size_t reserve_capacity = 64)
        : ctx_(ctx) {
        tasks_.reserve(reserve_capacity);
    }
    
    task_group(const task_group&) = delete;
    task_group& operator=(const task_group&) = delete;
    
    task_group(task_group&& other) noexcept
        : ctx_(other.ctx_)
        , tasks_(std::move(other.tasks_))
        , sweep_interval_(other.sweep_interval_)
        , spawn_count_(other.spawn_count_) {
        other.ctx_ = nullptr;
    }
    
    task_group& operator=(task_group&& other) noexcept {
        if (this != &other) {
            ctx_ = other.ctx_;
            tasks_ = std::move(other.tasks_);
            sweep_interval_ = other.sweep_interval_;
            spawn_count_ = other.spawn_count_;
            other.ctx_ = nullptr;
        }
        return *this;
    }
    
    ~task_group() noexcept = default;
    
    // -------------------------------------------------------------------------
    // Task Spawning
    // -------------------------------------------------------------------------
    
    void spawn(task<T>&& t) {
        t.start();
        tasks_.push_back(std::move(t));
        ++spawn_count_;
        
        if ((spawn_count_ & (sweep_interval_ - 1)) == 0) {
            sweep();
        }
    }
    
    template<typename... Tasks>
    void spawn_all(Tasks&&... tasks) {
        (spawn(std::forward<Tasks>(tasks)), ...);
    }
    
    // -------------------------------------------------------------------------
    // Lifetime Management
    // -------------------------------------------------------------------------
    
    size_t sweep() {
        const size_t before = tasks_.size();
        std::erase_if(tasks_, [](task<T>& t) { return t.done(); });
        return before - tasks_.size();
    }
    
    void set_sweep_interval(size_t interval) {
        size_t pow2 = 1;
        while (pow2 < interval) pow2 <<= 1;
        sweep_interval_ = pow2;
    }
    
    // -------------------------------------------------------------------------
    // Waiting / Joining
    // -------------------------------------------------------------------------
    
    task<> join_all(io_context& ctx, std::chrono::milliseconds poll = std::chrono::milliseconds(10));
    
    task<bool> join_all_timeout(io_context& ctx, std::chrono::milliseconds timeout);
    
    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------
    
    size_t size() const { return tasks_.size(); }
    
    size_t active_count() const {
        return std::count_if(tasks_.begin(), tasks_.end(),
                            [](const task<T>& t) { return !t.done(); });
    }
    
    bool all_done() const {
        return std::all_of(tasks_.begin(), tasks_.end(),
                          [](const task<T>& t) { return t.done(); });
    }
    
    bool empty() const { return tasks_.empty(); }
    
    uint64_t total_spawned() const { return spawn_count_; }
    
    std::vector<task<T>>& tasks() { return tasks_; }
    const std::vector<task<T>>& tasks() const { return tasks_; }

private:
    io_context* ctx_;
    std::vector<task<T>> tasks_;
    size_t sweep_interval_ = 1024;
    uint64_t spawn_count_ = 0;
};

// Out-of-line definitions (need full io_context definition)
template<typename T>
task<> task_group<T>::join_all(io_context& ctx, std::chrono::milliseconds poll) {
    while (!all_done()) {
        sweep();
        co_await async_sleep(ctx, poll);
    }
    sweep();
}

template<typename T>
task<bool> task_group<T>::join_all_timeout(io_context& ctx, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    
    while (!all_done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        sweep();
        co_await async_sleep(ctx, std::chrono::milliseconds(10));
    }
    
    sweep();
    co_return true;
}

using void_task_group = task_group<>;

} // namespace aio
