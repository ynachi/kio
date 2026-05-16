#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace URing
{

// Segmented single-producer/single-consumer queue.
//
// The producer thread owns enqueue/emplace. The consumer thread owns
// try_dequeue, try_dequeue_batch, drain_batch, and consumer_empty. Destruction
// is only valid after both sides have stopped touching the queue.
template <typename T, std::size_t SegmentCapacity = 256>
class SpscQueue
{
    static_assert(SegmentCapacity > 0);
    static_assert(std::is_move_constructible_v<T>);

    struct Segment
    {
        std::array<std::optional<T>, SegmentCapacity> slots{};
        std::atomic<std::size_t> published{0};
        std::atomic<Segment*> next{nullptr};
    };

    struct alignas(64) ProducerState
    {
        Segment* segment = nullptr;
        std::size_t offset = 0;
    };

    struct alignas(64) ConsumerState
    {
        Segment* segment = nullptr;
        std::size_t offset = 0;
    };

    ProducerState producer_;
    ConsumerState consumer_;

public:
    SpscQueue()
    {
        auto* initial = new Segment();
        producer_.segment = initial;
        consumer_.segment = initial;
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    ~SpscQueue()
    {
        Segment* segment = consumer_.segment;
        while (segment != nullptr)
        {
            Segment* next = segment->next.load(std::memory_order_relaxed);
            delete segment;
            segment = next;
        }
    }

    [[nodiscard]] bool enqueue(T value) { return emplace(std::move(value)); }

    template <typename... Args>
    [[nodiscard]] bool emplace(Args&&... args)
    {
        if (producer_.offset == SegmentCapacity)
        {
            auto* segment = new (std::nothrow) Segment();
            if (segment == nullptr)
            {
                return false;
            }

            producer_.segment->next.store(segment, std::memory_order_release);
            producer_.segment = segment;
            producer_.offset = 0;
        }

        const std::size_t offset = producer_.offset;
        producer_.segment->slots[offset].emplace(std::forward<Args>(args)...);
        producer_.offset = offset + 1;
        producer_.segment->published.store(producer_.offset, std::memory_order_release);
        return true;
    }

    template <typename U = T>
        requires std::is_move_assignable_v<U>
    [[nodiscard]] bool try_dequeue(T& out)
    {
        T* item = peek_next();
        if (item == nullptr)
        {
            return false;
        }

        out = std::move(*item);
        pop_peeked();
        return true;
    }

    template <typename OutputIt>
    std::size_t try_dequeue_batch(OutputIt out, const std::size_t max_items)
    {
        std::size_t count = 0;

        while (count < max_items)
        {
            Segment* segment = consumer_.segment;
            const std::size_t published = segment->published.load(std::memory_order_acquire);

            if (consumer_.offset >= published)
            {
                if (!advance_segment_if_exhausted())
                {
                    break;
                }
                continue;
            }

            const std::size_t available = std::min(published - consumer_.offset, max_items - count);
            for (std::size_t i = 0; i < available; ++i)
            {
                auto& slot = segment->slots[consumer_.offset + i];
                *out++ = std::move(*slot);
                slot.reset();
            }

            consumer_.offset += available;
            count += available;
            (void)advance_segment_if_exhausted();
        }

        return count;
    }

    template <typename Fn>
    std::size_t drain_batch(const std::size_t max_items, Fn&& fn)
    {
        std::size_t count = 0;

        while (count < max_items)
        {
            Segment* segment = consumer_.segment;
            const std::size_t published = segment->published.load(std::memory_order_acquire);

            if (consumer_.offset >= published)
            {
                if (!advance_segment_if_exhausted())
                {
                    break;
                }
                continue;
            }

            const std::size_t available = std::min(published - consumer_.offset, max_items - count);
            for (std::size_t i = 0; i < available; ++i)
            {
                auto& slot = segment->slots[consumer_.offset];
                T value = std::move(*slot);
                slot.reset();
                ++consumer_.offset;
                ++count;
                fn(std::move(value));
            }

            (void)advance_segment_if_exhausted();
        }

        return count;
    }

    [[nodiscard]] bool consumer_empty() const noexcept
    {
        const Segment* segment = consumer_.segment;
        const std::size_t published = segment->published.load(std::memory_order_acquire);
        if (consumer_.offset < published)
        {
            return false;
        }

        return segment->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    [[nodiscard]] T* peek_next()
    {
        while (true)
        {
            Segment* segment = consumer_.segment;
            const std::size_t published = segment->published.load(std::memory_order_acquire);
            if (consumer_.offset < published)
            {
                return &*segment->slots[consumer_.offset];
            }

            if (!advance_segment_if_exhausted())
            {
                return nullptr;
            }
        }
    }

    void pop_peeked()
    {
        consumer_.segment->slots[consumer_.offset].reset();
        ++consumer_.offset;
        (void)advance_segment_if_exhausted();
    }

    [[nodiscard]] bool advance_segment_if_exhausted()
    {
        if (consumer_.offset != SegmentCapacity)
        {
            return false;
        }

        Segment* current = consumer_.segment;
        Segment* next = current->next.load(std::memory_order_acquire);
        if (next == nullptr)
        {
            return false;
        }

        consumer_.segment = next;
        consumer_.offset = 0;
        delete current;
        return true;
    }
};

}  // namespace URing
