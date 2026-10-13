#include "kio/buffer_pool.hpp"

#include <algorithm>
#include <cerrno>
#include <system_error>

namespace kio
{
RegisteredBufferPool::RegisteredBufferPool(const size_t slot_size, const uint32_t slots)
    : slot_size_(slot_size), slots_(slots)
{
    // Invariant 2: slot_size must be a power of two >= 4096 dividing 1 GiB
    if (slot_size < kPageSize || (slot_size & (slot_size - 1)) != 0 || (kOneGiB % slot_size) != 0)
    {
        throw std::invalid_argument("slot_size must be a power of two >= 4096 that evenly divides 1 GiB");
    }

    if (slots == 0)
    {
        throw std::invalid_argument("slots must be > 0");
    }

    // No overflow check: slot_size is validated <= 1 GiB above and
    // slots fits uint32_t, so the product is bounded below 2^62 -- the
    // former __builtin_mul_overflow branch was unreachable.
    total_bytes_ = slot_size_ * size_t{slots_};

    // Invariant 1: align BOTH length and base to 2 MiB for THP. Map one
    // extra 2 MiB, carve an aligned base out of the mapping, and return
    // the head and tail slivers so the final mapping is exactly
    // [base_, base_ + aligned_total_bytes_) on a 2 MiB boundary.
    aligned_total_bytes_ = (total_bytes_ + kHugePageSize - 1) & ~(kHugePageSize - 1);

    // Pre-reserve the free list so release()/try_acquire() are noexcept.
    free_slots_.reserve(slots_);
    for (uint32_t i = slots_; i != 0; --i)
    {
        free_slots_.push_back(i - 1);
    }

    const size_t map_bytes = aligned_total_bytes_ + kHugePageSize;
    void* mapping = mmap(nullptr, map_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
    {
        throw std::system_error(errno, std::system_category(), "mmap buffer pool failed");
    }

    const auto raw = reinterpret_cast<uintptr_t>(mapping);
    const auto aligned = (raw + kHugePageSize - 1) & ~(kHugePageSize - 1);
    base_ = reinterpret_cast<std::byte*>(aligned);
    (void)::munmap(mapping, aligned - raw);
    (void)::munmap(base_ + aligned_total_bytes_, (raw + map_bytes) - (aligned + aligned_total_bytes_));

    (void)madvise(base_, aligned_total_bytes_, MADV_HUGEPAGE);
}

RegisteredBufferPool::~RegisteredBufferPool() noexcept
{
    close();

    if (base_ != nullptr)
    {
        (void)::munmap(base_, aligned_total_bytes_);
        base_ = nullptr;
    }
}

BufferLease RegisteredBufferPool::try_acquire() noexcept
{
    // Surplus rule (Invariants 4 & 5): free-list entries covered by
    // reserved_ are spoken for by already-woken waiters. A single
    // comparison covers closed, exhausted, and all-reserved.
    if (closed_ || free_slots_.size() <= reserved_)
    {
        return {};
    }

    // LIFO pop: returns the most recently used slot for CPU cache warmth
    const uint32_t ordinal = free_slots_.back();
    free_slots_.pop_back();

    return make_lease(ordinal);
}

void RegisteredBufferPool::release(const uint32_t ordinal) noexcept
{
    CHECK(ordinal < slots_) << "release of ordinal " << ordinal << " outside the pool";

    // LIFO push first; the woken waiter consumes it from the free list
    // (wake-and-retry), so release() stays branch-simple.
    free_slots_.push_back(ordinal);

    if (!closed_ && !waiters_.empty())
    {
        std::coroutine_handle<> h;
        if (waiters_.pop_batch(&h, 1) == 1)
        {
            // The surplus slot is spoken for until the waiter resumes.
            ++reserved_;
            CHECK(wake_fn_ != nullptr) << "pool wake with no wake callback installed";
            wake_fn_(wake_ctx_, h);
        }
    }
}

void RegisteredBufferPool::close() noexcept
{
    if (closed_)
    {
        return;
    }
    closed_ = true;

    // Waiters already woken were popped at wake time and resume into
    // the closed_ branch of await_resume on their own; only still-parked
    // handles need a wake.
    std::coroutine_handle<> h;
    while (waiters_.pop_batch(&h, 1) == 1)
    {
        CHECK(wake_fn_ != nullptr) << "pool close with waiters but no wake callback";
        wake_fn_(wake_ctx_, h);
    }
}

uint16_t RegisteredBufferPool::append_regions(std::vector<iovec>& table)
{
    const size_t regions = (total_bytes_ + kOneGiB - 1) / kOneGiB;
    CHECK(table.size() + regions <= 65535)
        << "io_uring registered-buffer index space exhausted (SQE buf_index is uint16)";

    first_index_ = static_cast<uint16_t>(table.size());

    // Invariants 2 & 3: slice the contiguous mapping into <= 1 GiB chunks
    for (size_t offset = 0; offset < total_bytes_; offset += kOneGiB)
    {
        const size_t len = std::min(kOneGiB, total_bytes_ - offset);
        table.push_back(iovec{.iov_base = base_ + offset, .iov_len = len});
    }

    return first_index_;
}
}  // namespace kio
