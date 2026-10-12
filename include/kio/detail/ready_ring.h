#pragma once

#include "absl/log/check.h"

#include <algorithm>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace kio::detail
{
static_assert(std::is_trivially_copyable_v<std::coroutine_handle<>>, "ReadyRing memcpy-pops handle storage");

// Context's ready list: a fixed-slot FIFO ring of coroutine handles with
// power-of-two capacity, grown by doubling when full.
//
// Loop-thread only, like everything that touches it (post/run/reap): no
// atomics, no locks. head_/tail_ are monotonic counters; the element
// count is always tail_ - head_ and the physical slot is index & (cap_ -
// 1). Never compare head_ < tail_: after 2^64 operations the subtraction
// is still correct and the comparison would not be.
//
// Capacity policy: no fixed capacity is provably sufficient (spawn()
// fan-out and nested reaps make per-epoch demand unbounded), so full
// means grow, not reject. The fence below only turns a runaway enqueue
// into a diagnosable abort. Handles are trivially copyable, so growing
// is a memcpy, and callers pop into their own buffer before resuming --
// resume runs user code that may push and therefore rebase this ring.
class ReadyRing
{
public:
    static constexpr std::uint64_t kInitialCapacity = 1024;
    // 8 MiB of handles (1<<20): a crash-vs-OOM fence, not a scheduling bound.
    static constexpr std::uint64_t kMaxCapacity = 1ULL << 20;

    explicit ReadyRing(const std::uint64_t initial_capacity = kInitialCapacity)
        : cap_(initial_capacity), slots_(std::make_unique<std::coroutine_handle<>[]>(initial_capacity))
    {
        CHECK(initial_capacity > 0 && (initial_capacity & (initial_capacity - 1)) == 0)
            << "ReadyRing capacity must be a positive power of two, got " << initial_capacity;
    }

    // Not noexcept: allocates iff this push fills the ring (grow).
    void push(const std::coroutine_handle<> handle)
    {
        CHECK(handle.address() != nullptr) << "push of a null coroutine handle";
        if (size() == cap_)
        {
            grow();
        }
        slots_[tail_ & (cap_ - 1)] = handle;
        ++tail_;
    }

    // Bulk FIFO pop: k = min(max, size) handles copied to out (two memcpy
    // legs across the wrap), head advances once. Copy-out is the safety
    // contract: callers resume from out, running user code that may push
    // and grow -- i.e. reallocate and rebase -- this ring underneath.
    std::uint64_t pop_batch(std::coroutine_handle<>* out, const std::uint64_t max) noexcept
    {
        const std::uint64_t k = std::min(max, size());
        const std::uint64_t first = std::min(k, cap_ - (head_ & (cap_ - 1)));
        std::memcpy(out, &slots_[head_ & (cap_ - 1)], first * sizeof(std::coroutine_handle<>));
        std::memcpy(out + first, slots_.get(), (k - first) * sizeof(std::coroutine_handle<>));
        head_ += k;
        return k;
    }

    [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }

    [[nodiscard]] std::uint64_t size() const noexcept { return tail_ - head_; }

private:
    // Double the capacity and unwrap the logical order into the new
    // slots, rebasing head_ to 0. Trivially copyable handles make this a
    // two-leg memcpy; no outstanding references into slots_ exist (the
    // only caller that runs code, pop_batch, copies out first).
    void grow()
    {
        const std::uint64_t new_cap = cap_ * 2;
        CHECK(new_cap <= kMaxCapacity) << "ReadyRing refuses to grow past " << kMaxCapacity
                                       << " entries (runaway enqueue?)";
        auto slots = std::make_unique<std::coroutine_handle<>[]>(new_cap);
        const std::uint64_t n = size();
        const std::uint64_t first = std::min(n, cap_ - (head_ & (cap_ - 1)));
        std::memcpy(slots.get(), &slots_[head_ & (cap_ - 1)], first * sizeof(std::coroutine_handle<>));
        std::memcpy(slots.get() + first, slots_.get(), (n - first) * sizeof(std::coroutine_handle<>));
        slots_ = std::move(slots);
        cap_ = new_cap;
        head_ = 0;
        tail_ = n;
    }

    std::uint64_t head_ = 0;
    std::uint64_t tail_ = 0;
    std::uint64_t cap_;
    std::unique_ptr<std::coroutine_handle<>[]> slots_;
};
}  // namespace kio::detail
