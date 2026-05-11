#pragma once
#include <array>
#include <cstddef>
#include <iterator>

namespace URing
{
class CoroAllocator
{
    static constexpr std::size_t kBuckets[] = {128, 256, 512, 1024, 2048};
    static constexpr std::size_t kNumBuckets = std::size(kBuckets);
    static constexpr std::size_t kRefillBatchSize = 32;

    struct alignas(std::max_align_t) Block
    {
        Block* next;
    };

    /// TL free list of blocks
    static thread_local std::array<Block*, kNumBuckets> tl_blocks;

    // Optimized unrolled linear search
    static constexpr std::size_t get_bucket_index(const std::size_t size) noexcept
    {
        if (size <= 128)
            return 0;
        if (size <= 256)
            return 1;
        if (size <= 512)
            return 2;
        if (size <= 1024)
            return 3;
        if (size <= 2048)
            return 4;
        return kNumBuckets;
    }

    // Refill a bucket's free list with a batch of blocks
    static Block* refill_bucket(const std::size_t bucket_idx) noexcept
    {
        const std::size_t block_size = kBuckets[bucket_idx];
        // Allocate contiguous memory for the batch (better cache locality)
        void* memory = ::operator new(block_size * kRefillBatchSize);

        // Build the free-list chain within the batch
        auto* head = static_cast<Block*>(memory);
        auto* current = head;
        for (std::size_t i = 1; i < kRefillBatchSize; ++i)
        {
            current->next = reinterpret_cast<Block*>(static_cast<char*>(memory) + i * block_size);
            current = current->next;
        }
        current->next = nullptr;
        return head;
    }

public:
    static void prewarm(const std::size_t bucket_idx, const std::size_t count)
    {
        if (bucket_idx >= kNumBuckets)
            return;

        const std::size_t size = kBuckets[bucket_idx];
        for (std::size_t i = 0; i < count; ++i)
        {
            void* ptr = ::operator new(size);
            // Puts it into the free_list
            deallocate(ptr, size);
        }
    }

    static void* allocate(const std::size_t size) noexcept
    {
        if (const auto idx = get_bucket_index(size); idx < kNumBuckets)
        {
            // Fast path: pop from thread-local free list
            if (Block* blk = tl_blocks[idx])
            {
                tl_blocks[idx] = blk->next;
                return blk;
            }
            // Slow path: refill the bucket
            Block* batch = refill_bucket(idx);
            // Return one block to caller, rest go to free list
            tl_blocks[idx] = batch->next;
            return batch;
        }
        // Fallback for oversized frames
        return ::operator new(size);
    }

    static void deallocate(void* ptr, const std::size_t size) noexcept
    {
        if (const auto idx = get_bucket_index(size); idx < kNumBuckets)
        {
            auto* blk = static_cast<Block*>(ptr);
            blk->next = tl_blocks[idx];
            tl_blocks[idx] = blk;
            return;
        }
        ::operator delete(ptr);
    }
};

inline thread_local std::array<CoroAllocator::Block*, CoroAllocator::kNumBuckets> CoroAllocator::tl_blocks = {nullptr};
}  // namespace URing