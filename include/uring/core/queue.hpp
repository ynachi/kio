#pragma once
#include <atomic>

#include "uring/task.hpp"

namespace URing
{
/// MPSC intrusive queue for coroutines
class CoroQueue
{
public:
    CoroQueue()
    {
        stub_.next.store(nullptr, std::memory_order_relaxed);
        head_.store(&stub_, std::memory_order_relaxed);
        tail_ = &stub_;
    }

    void enqueue(task_promise_base* node)
    {
        node->next.store(nullptr, std::memory_order_relaxed);
        task_promise_base* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    task_promise_base* dequeue()
    {
        task_promise_base* tail = tail_;
        task_promise_base* next = tail->next.load(std::memory_order_acquire);

        if (tail == &stub_)
        {
            if (next == nullptr)
            {
                return nullptr;
            }
            tail_ = next;
            tail = next;
            next = next->next.load(std::memory_order_acquire);
        }

        if (next != nullptr)
        {
            tail_ = next;
            return tail;
        }

        task_promise_base* head = head_.load(std::memory_order_acquire);
        if (tail != head)
        {
            return nullptr;
        }

        enqueue(&stub_);
        next = tail->next.load(std::memory_order_acquire);
        if (next != nullptr)
        {
            tail_ = next;
            return tail;
        }
        return nullptr;
    }

    template <typename Fn>
    std::size_t drain(Fn&& fn, const std::size_t max_count = std::numeric_limits<std::size_t>::max())
    {
        std::size_t count = 0;
        while (count < max_count)
        {
            task_promise_base* node = dequeue();
            if (node == nullptr)
            {
                break;
            }
            fn(node);
            ++count;
        }
        return count;
    }

    bool empty() const noexcept{ return head_.load(std::memory_order_acquire) == tail_; }

private:
    alignas(64) std::atomic<task_promise_base*> head_;
    alignas(64) task_promise_base* tail_;
    // Dummy node to prevent empty-queue race conditions
    task_promise_base stub_{};
};

}  // namespace URing