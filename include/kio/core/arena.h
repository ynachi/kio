#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <vector>

namespace kio
{
class Arena
{
public:
    static constexpr std::size_t kBlockSize = 4096;

    // block sz should be a power of two
    explicit Arena(const std::size_t block_size = kBlockSize)
        : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0), block_size_(block_size)
    {
    }

    ~Arena()
    {
        for (std::byte* block : blocks_)
        {
            std::free(block);
        }
    }

    // Disable copy and move
    Arena(const Arena&) = delete;

    Arena& operator=(const Arena&) = delete;

    std::byte* AllocateAligned(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));

    [[nodiscard]] std::size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

private:
    std::byte* AllocateFallback(std::size_t bytes, std::size_t alignment);

    std::byte* AllocateNewBlock(std::size_t bytes, std::size_t alignment);

    std::byte* alloc_ptr_;
    std::size_t alloc_bytes_remaining_;
    std::vector<std::byte*> blocks_;
    std::atomic<std::size_t> memory_usage_;
    std::size_t block_size_;
};

// STL a
template <class T>
class ArenaAllocator
{
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <class U>
    struct rebind
    {
        typedef ArenaAllocator<U> other;
    };

    // Pointer to the actual arena
    Arena* arena_;

    explicit ArenaAllocator(Arena& arena) noexcept : arena_(&arena) {}

    template <class U>
    explicit ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena_(other.arena_)
    {
    }

    T* allocate(const std::size_t n)
    {
        const std::size_t bytes = n * sizeof(T);
        const std::size_t alignment = alignof(T) > alignof(std::max_align_t) ? alignof(T) : alignof(std::max_align_t);
        return reinterpret_cast<T*>(arena_->AllocateAligned(bytes, alignment));
    }

    void deallocate(T* p, const std::size_t n) noexcept
    {
        // Intentional No-Op.
        // We generally don't free individual nodes.
        // The container will still call the destructors of the objects,
        // but the raw memory is held until the Arena itself is destroyed.
        (void)p;
        (void)n;
    }
};

// Allocator equality operators (required by STL)
// Two allocators are equal if they allocate from the exact same Arena instance.
template <class T, class U>
bool operator==(const ArenaAllocator<T>& lhs, const ArenaAllocator<U>& rhs) noexcept
{
    return lhs.arena_ == rhs.arena_;
}

template <class T, class U>
bool operator!=(const ArenaAllocator<T>& lhs, const ArenaAllocator<U>& rhs) noexcept
{
    return lhs.arena_ != rhs.arena_;
}
}  // namespace kio
