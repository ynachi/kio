#ifndef LOCK_FREE_QUEUE_H
#define LOCK_FREE_QUEUE_H

#include <atomic>
#include <cassert>
#include <new>  // for std::hardware_destructive_interference_size
#include <stdexcept>
#include <vector>

namespace kio
{

// Use 64 bytes cache line size for alignment to prevent false sharing
inline constexpr std::size_t kCacheLineSize = 64;

/**
 * @brief Bounded Lock-Free Multi-Producer Single-Consumer Queue
 * * Uses a ring buffer with sequence numbers to ensure thread safety
 * and correct Acquire-Release memory ordering.
 */
template <typename T>
class MPSCQueue
{
public:
    explicit MPSCQueue(size_t size) : buffer_(size), mask_(size - 1)
    {
        // Size must be a power of 2
        if (size == 0 || (size & (size - 1)) != 0)
        {
        }

        // Initialize sequence numbers
        // Initial sequence for slot i is i.
        // After write: i + 1
        // After read: i + size + 1 (generations)
        for (size_t i = 0; i < size; ++i)
        {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }

        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    // Producer Thread(s)
    bool TryPush(T&& data)
    {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;)
        {
            cell = &buffer_[pos & mask_];

            // ACQUIRE: Ensure we see the slot's state (empty/full) correctly
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)pos;

            if (dif == 0)
            {
                // Slot is empty and matches our position. Try to reserve it.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    break;  // Success, we own this slot now
                }
            }
            else if (dif < 0)
            {
                // Sequence is behind position, meaning queue is full or wrapped weirdly
                return false;
            }
            else
            {
                // Someone else advanced enqueue_pos_, update our view
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        // We own the slot. Write data.
        cell->data = std::move(data);

        // RELEASE: Commit the data write. Consumer will only see the sequence update
        // AFTER the data write is visible.
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Consumer Thread (Single Consumer)
    bool TryPop(T& data)
    {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        cell = &buffer_[pos & mask_];

        // ACQUIRE: Ensure we see the data written by producer
        size_t seq = cell->sequence.load(std::memory_order_acquire);
        // Logic: If seq == pos + 1, it means producer finished writing (pos -> pos + 1)
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);

        if (dif == 0)
        {
            // Data is ready.
            data = std::move(cell->data);

            // Release the slot back to producers.
            // Move sequence to next generation: pos + 1 -> pos + buffer_mask + 1
            // RELEASE: Ensure we are done reading before allowing overwrite
            cell->sequence.store(pos + mask_ + 1, std::memory_order_release);

            dequeue_pos_.store(pos + 1, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

private:
    struct Cell
    {
        std::atomic<size_t> sequence;
        T data;
    };

    std::vector<Cell> buffer_;
    const size_t mask_;

    // Padding to prevent false sharing between producer and consumer pointers
    alignas(kCacheLineSize) std::atomic<size_t> enqueue_pos_;
    alignas(kCacheLineSize) std::atomic<size_t> dequeue_pos_;
};

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

#endif  // LOCK_FREE_QUEUE_H
