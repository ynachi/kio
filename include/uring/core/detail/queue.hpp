#pragma once
#include <atomic>
#include <cstddef>
#include <limits>

#include "uring/core/task.hpp"

namespace kio::detail
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

    void enqueue(TaskPromiseBase* node)
    {
        node->next.store(nullptr, std::memory_order_relaxed);
        TaskPromiseBase* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    TaskPromiseBase* dequeue()
    {
        TaskPromiseBase* tail = tail_;
        TaskPromiseBase* next = tail->next.load(std::memory_order_acquire);

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

        if (const TaskPromiseBase* head = head_.load(std::memory_order_acquire); tail != head)
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
            TaskPromiseBase* node = dequeue();
            if (node == nullptr)
            {
                break;
            }
            fn(node);
            ++count;
        }
        return count;
    }

    /// Consumer-side only. tail_ always points at the next node dequeue() will
    /// return, so the queue is empty only if that is the stub with nothing linked
    /// behind it. Comparing head_ to tail_ instead reported "empty" while exactly
    /// one real node was still pending (head_ == tail_ == that node), which let
    /// the worker park with a task stranded in the queue.
    ///
    /// A producer between its exchange and its link store reads as empty; that is
    /// fine, because it calls wake() only after the link is published.
    bool empty() const noexcept
    {
        return tail_ == &stub_ && stub_.next.load(std::memory_order_acquire) == nullptr;
    }

private:
    alignas(64) std::atomic<TaskPromiseBase*> head_;
    alignas(64) TaskPromiseBase* tail_;
    // Dummy node to prevent empty-queue race conditions. Producers store to
    // stub_.next when it is the last node, so keep it off the consumer's tail_ line.
    alignas(64) TaskPromiseBase stub_{};
};

}  // namespace URing::detail