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
    /*
     * =======================================================================
     * TRIVIAL RING BUFFER (Lock-Free, Single-Producer/Single-Consumer style)
     * =======================================================================
     *
     * A highly optimized, dynamically growing FIFO queue for trivially
     * copyable types. It uses a flat array but behaves like a circular buffer.
     *
     * KEY MECHANICS:
     * 1. POWER-OF-TWO CAPACITY: Capacity is always 2^N. This allows us to
     *    replace the slow modulo operator (%) with a fast bitwise AND (&).
     *    Physical Index = Logical Index & (Capacity - 1).
     *
     * 2. MONOTONIC COUNTERS: `head_` and `tail_` only ever increase. They
     *    NEVER reset to 0.
     *    - Size is simply: tail_ - head_.
     *    - This avoids the classic ring-buffer bug where comparing
     *      `head < tail` fails after 2^64 operations due to integer wrap.
     *
     * 3. TWO-LEG MEMCPY: Because the logical queue wraps around the physical
     *    array boundary, reading or writing a batch often requires two
     *    separate memcpy calls (one to the end of the array, one from the start).
     *
     * =======================================================================
     * ASCII DIAGRAMS
     * =======================================================================
     *
     * STATE 1: Normal Operation (No Wrap)
     * Logical:  [ A ] [ B ] [ C ] [ D ]
     * head_ = 0, tail_ = 4. Size = 4.
     *
     * Physical Array (cap = 8):
     * Index:    0   1   2   3   4   5   6   7
     * Data:   [ A | B | C | D |   |   |   |   ]
     *           ^               ^
     *         head_           tail_
     *
     * -----------------------------------------------------------------------
     *
     * STATE 2: The "Wrap-Around" (The Ring Effect)
     * We pushed 4 more items (E, F, G, H). tail_ reached the end and wrapped.
     * Logical:  [ A ] [ B ] [ C ] [ D ] [ E ] [ F ] [ G ] [ H ]
     * head_ = 0, tail_ = 8. Size = 8.
     *
     * Physical Array (cap = 8):
     * Index:    0   1   2   3   4   5   6   7
     * Data:   [ A | B | C | D | E | F | G | H ]
     *           ^                               ^
     *         head_                           tail_
     *
     * Now we pop 4 items (A,B,C,D). head_ advances.
     * Then we push 2 more (I, J). tail_ wraps back to index 0!
     *
     * Logical:  [ E ] [ F ] [ G ] [ H ] [ I ] [ J ]
     * head_ = 4, tail_ = 10. Size = 6.
     *
     * Physical Array (cap = 8):
     * Index:    0   1   2   3   4   5   6   7
     * Data:   [ I | J |   |   | E | F | G | H ]
     *           ^               ^
     *         tail_           head_
     *
     * -----------------------------------------------------------------------
     *
     * STATE 3: Two-Leg Memcpy (Handling the physical split)
     * If we want to pop_batch(4) from the wrapped state above, the data is
     * physically split. We must copy it in two legs:
     *
     * Physical Array: [ I | J |   |   | E | F | G | H ]
     *                   <--Leg 2-->   <-----Leg 1----->
     *                   (idx 0 to 1)  (idx 4 to 7)
     *
     * =======================================================================
     */

    template <typename T>
    class TrivialRingBuffer
    {
        // ENFORCE THE CONTRACT: We use memcpy, so the type MUST be trivially copyable.
        // This prevents silent data corruption if someone tries to use std::string, etc.
        static_assert(std::is_trivially_copyable_v<T>,
                      "TrivialRingBuffer requires trivially copyable types for safe memcpy");

    public:
        static constexpr std::uint64_t kInitialCapacity = 1024;
        // 8 MiB of handles (assuming 8-byte pointers): a crash-vs-OOM fence.
        static constexpr std::uint64_t kMaxCapacity = 1ULL << 20U;

        explicit TrivialRingBuffer(std::uint64_t initial_capacity = kInitialCapacity)
            : cap_(checked_capacity(initial_capacity)),
              // make_unique_for_overwrite avoids zero-initializing the array,
              // which is a waste of CPU cycles since we will overwrite it anyway.
              slots_(std::make_unique_for_overwrite<T[]>(initial_capacity))
        {
        }

        // Not noexcept: allocates iff this push fills the ring (triggers grow).
        void push(const T& item)
        {
            if (size() == cap_)
            {
                grow();
            }
            // Bitwise AND maps the logical tail_ to the physical array index.
            // Equivalent to: tail_ % cap_, but much faster.
            slots_[tail_ & (cap_ - 1)] = item;
            ++tail_;
        }

        // Bulk FIFO pop: copies up to `max` items into `out`.
        // Returns the actual number of items copied.
        std::uint64_t pop_batch(T* out, std::uint64_t max) noexcept
        {
            const std::uint64_t k = std::min(max, size());
            if (k == 0) return 0;

            // Calculate how many items are contiguous from head_ to the end of the array.
            const std::uint64_t first_leg = std::min(k, cap_ - (head_ & (cap_ - 1)));

            // LEG 1: Copy from head_ to the physical end of the array (or until we hit `k`).
            std::memcpy(out, &slots_[head_ & (cap_ - 1)], first_leg * sizeof(T));

            // LEG 2: If the batch wrapped around the physical boundary, copy the remainder
            // from the physical start of the array (index 0).
            std::memcpy(out + first_leg, slots_.get(), (k - first_leg) * sizeof(T));

            head_ += k;
            return k;
        }

        [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }

        // Because head_ and tail_ are monotonic and unsigned, subtraction
        // is mathematically correct even if they wrap past 2^64.
        [[nodiscard]] std::uint64_t size() const noexcept { return tail_ - head_; }

    private:
        // Validates capacity before allocation to ensure we fail with a clear
        // CHECK message rather than a confusing std::bad_alloc.
        static std::uint64_t checked_capacity(std::uint64_t cap)
        {
            CHECK(cap > 0 && (cap & (cap - 1)) == 0)
                << "Capacity must be a positive power of two, got " << cap;
            return cap;
        }

        // Doubles the capacity. Crucially, it "unwraps" the logical order
        // into the new array so that head_ resets to 0.
        void grow()
        {
            const std::uint64_t new_cap = cap_ * 2;
            CHECK(new_cap <= kMaxCapacity)
                << "TrivialRingBuffer refuses to grow past " << kMaxCapacity << " entries (runaway enqueue?)";

            auto new_slots = std::make_unique_for_overwrite<T[]>(new_cap);
            const std::uint64_t n = size();

            // Just like pop_batch, we must copy the data in two legs if it's wrapped.
            const std::uint64_t first_leg = std::min(n, cap_ - (head_ & (cap_ - 1)));

            std::memcpy(new_slots.get(), &slots_[head_ & (cap_ - 1)], first_leg * sizeof(T));
            std::memcpy(new_slots.get() + first_leg, slots_.get(), (n - first_leg) * sizeof(T));

            // Swap in the new array and reset the counters.
            slots_ = std::move(new_slots);
            cap_ = new_cap;
            head_ = 0;
            tail_ = n;
        }

        std::uint64_t head_ = 0; // Monotonic read index
        std::uint64_t tail_ = 0; // Monotonic write index
        std::uint64_t cap_; // Always a power of 2
        std::unique_ptr<T[]> slots_;
    };

    // Alias for the specific use-case if the rest of the codebase expects it
    using ReadyRing = TrivialRingBuffer<std::coroutine_handle<>>;
} // namespace kio::detail
