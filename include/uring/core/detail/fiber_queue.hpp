#pragma once
#include <atomic>
#include <cstddef>
#include <limits>

#include "uring/core/fiber.hpp"

namespace URing::detail
{

/// Vyukov MPSC intrusive queue for FiberContext pointers.
///
/// Mirrors CoroQueue exactly, replacing TaskPromiseBase* with FiberContext*
/// and using the FiberContext::next_queued atomic link instead of
/// TaskPromiseBase::next.
///
/// Thread-safety contract (same as CoroQueue):
///   - enqueue() is safe to call from any thread concurrently.
///   - dequeue() / drain() must only be called from the single consumer
///     thread that owns the target IO (the thread running tick()).
class FiberQueue
{
public:
    FiberQueue()
    {
        stub_.next_queued.store(nullptr, std::memory_order_relaxed);
        head_.store(&stub_, std::memory_order_relaxed);
        tail_ = &stub_;
    }

    void enqueue(FiberContext* node)
    {
        node->next_queued.store(nullptr, std::memory_order_relaxed);
        FiberContext* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next_queued.store(node, std::memory_order_release);
    }

    FiberContext* dequeue()
    {
        FiberContext* tail = tail_;
        FiberContext* next = tail->next_queued.load(std::memory_order_acquire);

        if (tail == &stub_)
        {
            if (next == nullptr)
            {
                return nullptr;
            }
            tail_ = next;
            tail  = next;
            next  = next->next_queued.load(std::memory_order_acquire);
        }

        if (next != nullptr)
        {
            tail_ = next;
            return tail;
        }

        if (const FiberContext* head = head_.load(std::memory_order_acquire); tail != head)
        {
            return nullptr;
        }

        enqueue(&stub_);
        next = tail->next_queued.load(std::memory_order_acquire);
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
            FiberContext* node = dequeue();
            if (node == nullptr)
            {
                break;
            }
            fn(node);
            ++count;
        }
        return count;
    }

    bool empty() const noexcept { return head_.load(std::memory_order_acquire) == tail_; }

private:
    alignas(64) std::atomic<FiberContext*> head_;
    alignas(64) FiberContext*              tail_;
    FiberContext                           stub_{FiberContext::StubTag{}};
};

}  // namespace URing::detail
