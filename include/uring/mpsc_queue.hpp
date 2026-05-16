#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <limits>
#include <utility>

namespace URing
{

struct MpscNode
{
    std::atomic<MpscNode*> next{nullptr};
};

// Intrusive many-producer/single-consumer queue.
//
// Producers may call push from any thread. The single consumer owns try_pop and
// drain. A nullptr pop can mean either empty or a producer is between exchange
// and link publication; callers should retry on a later tick instead of
// spinning.
class MpscQueue
{
    alignas(64) std::atomic<MpscNode*> producer_end_;
    alignas(64) MpscNode* consumer_end_;
    MpscNode stub_;

public:
    MpscQueue() noexcept : producer_end_(&stub_), consumer_end_(&stub_) {}

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    ~MpscQueue() noexcept = default;

    void push(MpscNode* node) noexcept
    {
        node->next.store(nullptr, std::memory_order_relaxed);

        MpscNode* prev = producer_end_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    [[nodiscard]] MpscNode* try_pop() noexcept
    {
        for (;;)
        {
            MpscNode* tail = consumer_end_;
            MpscNode* next = tail->next.load(std::memory_order_acquire);

            if (next != nullptr)
            {
                consumer_end_ = next;
                tail->next.store(nullptr, std::memory_order_relaxed);

                if (tail != &stub_)
                {
                    return tail;
                }

                continue;
            }

            MpscNode* head = producer_end_.load(std::memory_order_acquire);
            if (tail != head)
            {
                return nullptr;
            }

            if (tail == &stub_)
            {
                return nullptr;
            }

            push(&stub_);

            next = tail->next.load(std::memory_order_acquire);
            if (next == nullptr)
            {
                return nullptr;
            }

            consumer_end_ = next;
            tail->next.store(nullptr, std::memory_order_relaxed);
            return tail;
        }
    }

    template <typename Func>
        requires std::invocable<Func, MpscNode*>
    std::size_t drain(Func&& func, std::size_t max_count = std::numeric_limits<std::size_t>::max()) noexcept
    {
        std::size_t count = 0;

        while (count < max_count)
        {
            MpscNode* node = try_pop();
            if (node == nullptr)
            {
                break;
            }

            std::forward<Func>(func)(node);
            ++count;
        }

        return count;
    }
};

}  // namespace URing
