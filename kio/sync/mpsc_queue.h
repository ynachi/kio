// Created by Yao ACHI on 19/10/2025.
//

#ifndef KIO_LOCK_FREE_QUEUE_H
#define KIO_LOCK_FREE_QUEUE_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace kio
{
/**
 * @brief A bounded, lock-free, multi-producer, single-consumer (MPSC) queue.
 *
 * Dmitry Vyukov's bounded MPSC queue (sequence per-slot).
 *
 * Supports non-default-constructible and move-only T by using placement-new
 * inside per-cell uninitialized storage.
 */
template<typename T>
class MPSCQueue
{
    static constexpr size_t kCacheLineSize = 64;

    // NOLINTBEGIN(
    //   misc-non-private-member-variables-in-classes,
    //   cppcoreguidelines-avoid-c-arrays,
    //   modernize-avoid-c-arrays,
    //   cppcoreguidelines-pro-type-reinterpret-cast
    //   readability-magic-numbers
    // )
    struct Cell
    {
        std::atomic<size_t> sequence;
        // Uninitialized storage for T
        alignas(alignof(T)) unsigned char storage[sizeof(T)]{};

        Cell() noexcept { /* don't construct T */ }

        // Access pointer to T in storage
        T *DataPtr() noexcept { return reinterpret_cast<T *>(storage); }
        const T *DataPtr() const noexcept { return reinterpret_cast<const T *>(storage); }

        // Construct T in-place using forwarding args
        template<typename... Args>
        void ConstructInPlace(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
        {
            ::new (static_cast<void *>(storage)) T(std::forward<Args>(args)...);
        }

        // Destroy the T in-place
        void DestroyInPlace() noexcept { DataPtr()->~T(); }

        // Note: we intentionally do not define destructor that destroys T,
        // because we only want to destroy when we know an object was constructed.
    };
    // NOLINTEND

public:
    explicit MPSCQueue(size_t capacity) : buffer_(capacity), mask_(capacity - 1)
    {
        // Capacity must be power of 2
        assert(capacity > 0 && (capacity & (capacity - 1)) == 0 && "Capacity must be a power of 2");

        // Initialize sequence numbers for each slot (no T construction)
        for (size_t i = 0; i < capacity; ++i)
        {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }

        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    MPSCQueue(const MPSCQueue &) = delete;
    MPSCQueue &operator=(const MPSCQueue &) = delete;
    MPSCQueue(MPSCQueue &&) = delete;
    MPSCQueue &operator=(MPSCQueue &&) = delete;

    ~MPSCQueue()
    {
        // Drain any remaining elements using the normal pop mechanism
        if constexpr (std::is_default_constructible_v<T>)
        {
            T item;
            while (TryPop(item))
            {
                // Item is properly moved out and the in-place object is destroyed
            }
        }
        else
        {
            // For non-default-constructible types, we need to manually destroy remaining objects
            const size_t head = dequeue_pos_.load(std::memory_order_relaxed);
            const size_t tail = enqueue_pos_.load(std::memory_order_relaxed);

            for (size_t i = head; i < tail; ++i)
            {
                Cell &cell = buffer_[i & mask_];

                // Check if this cell contains a constructed object
                if (const size_t seq = cell.sequence.load(std::memory_order_relaxed); seq == i + 1)
                {
                    cell.destroy_in_place();
                    // Mark as empty for safety
                    cell.sequence.store(i + mask_ + 1, std::memory_order_relaxed);
                }
            }
        }
    }

    template<typename U>
    bool TryPush(U &&u)
    {
        Cell *cell{nullptr};
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;)
        {
            cell = &buffer_[pos & mask_];
            const size_t seq = cell->sequence.load(std::memory_order_acquire);

            if (const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos); diff == 0)
            {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                       std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                return false;
            }
            else
            {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        // Construct directly in cell storage from a forwarded argument
        cell->ConstructInPlace(std::forward<U>(u));

        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * try_pop for a single consumer. Moves the contained T into 'item' and destroys the in-place object.
     */
    bool TryPop(T &item)
    {
        Cell *cell{nullptr};
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;)
        {
            cell = &buffer_[pos & mask_];
            const size_t seq = cell->sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0)
            {
                // This slot has data ready to read
                // Advance dequeue position (single consumer, relaxed ok)
                dequeue_pos_.store(pos + 1, std::memory_order_relaxed);
                break;
            }
            if (diff < 0)
            {
                // slot not yet written -> queue empty
                return false;
            }
            // should not happen normally; refresh
            pos = dequeue_pos_.load(std::memory_order_relaxed);
        }

        // Move the data out of the cell
        item = std::move(*cell->DataPtr());

        // Destroy the in-place object now that we've moved it out
        cell->DestroyInPlace();

        // Mark the slot as available for producers again:
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);

        return true;
    }

    [[nodiscard]]
    size_t Capacity() const noexcept
    {
        return mask_ + 1;
    }

    [[nodiscard]]
    size_t SizeApprox() const noexcept
    {
        const size_t head = dequeue_pos_.load(std::memory_order_relaxed);
        const size_t tail = enqueue_pos_.load(std::memory_order_relaxed);
        return tail - head;
    }

private:
    std::vector<Cell> buffer_;
    const size_t mask_;

    alignas(kCacheLineSize) std::atomic<size_t> dequeue_pos_;
    alignas(kCacheLineSize) std::atomic<size_t> enqueue_pos_;
};

// NOLINTBEGIN(readability-magic-numbers)
constexpr size_t NextPowerOf2(size_t n)
{
    if (n == 0)
    {
        return 1;
    };
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    if constexpr (sizeof(size_t) == 8)
    {
        n |= n >> 32;
    }
    return n + 1;
    // NOLINTEND(readability-magic-numbers)
}
}  // namespace kio

#endif  // KIO_LOCK_FREE_QUEUE_H
