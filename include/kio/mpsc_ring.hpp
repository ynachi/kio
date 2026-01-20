#pragma once
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Bounded MPSC ring buffer using per-slot sequence numbers (Vyukov-style).
// - Multiple producers, single consumer
// - No allocations after construction
// - TSAN-friendly (handoff is via release-store / acquire-load on slot.seq)
//
// Requirements on T:
// - nothrow move-constructible (or nothrow constructible for the chosen emplace args)
template <class T>
class MpscRing
{
    struct Slot
    {
        std::atomic<std::size_t> seq;
        alignas(T) std::byte storage[sizeof(T)]{};
    };

public:
    explicit MpscRing(const std::size_t capacity)
        : cap_(std::bit_ceil(std::max<std::size_t>(2, capacity))),
          mask_(cap_ - 1),
          slots_(std::make_unique<Slot[]>(cap_))
    {
        for (std::size_t i = 0; i < cap_; ++i)
            slots_[i].seq.store(i, std::memory_order_relaxed);
    }

    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;

    ~MpscRing() noexcept
    {
        // Best-effort: destroy remaining items if any (consumer-only).
        consume_up_to([](T&&) noexcept {}, static_cast<std::size_t>(-1));
    }

    std::size_t capacity() const noexcept { return cap_; }

    template <class... Args>
    bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>)
        requires(std::is_nothrow_constructible_v<T, Args&&...>)
    {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;)
        {
            Slot& s = slots_[pos & mask_];
            const std::size_t seq = s.seq.load(std::memory_order_acquire);

            // When slot is free, seq == pos.
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0)
            {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    // We own this slot; construct payload, then publish.
                    ::new (static_cast<void*>(s.storage)) T(std::forward<Args>(args)...);

                    // Publish item: release so consumer acquire sees payload writes.
                    s.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed; pos updated, retry.
            }
            else if (diff < 0)
            {
                // Slot still occupied => queue full for this producer position.
                return false;
            }
            else
            {
                // Another producer advanced; refresh pos and retry.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>)
        requires(std::is_nothrow_move_constructible_v<T>)
    {
        return try_emplace(std::move(v));
    }

    // Single-consumer: pop one item and pass it to f(T&&).
    // Slot is freed BEFORE calling f (so producers don't get stuck behind long callbacks).
    template <class F>
    bool try_consume_one(F&& f) noexcept(noexcept(f(std::declval<T&&>())))
        requires(std::is_nothrow_move_constructible_v<T>)
    {
        Slot& s = slots_[dequeue_pos_ & mask_];

        if (const std::size_t seq = s.seq.load(std::memory_order_acquire); seq != dequeue_pos_ + 1)
            return false; // empty

        T* p = std::launder(reinterpret_cast<T*>(s.storage));

        // Move out, destroy an in-slot object, then free slot.
        T tmp = std::move(*p);
        p->~T();

        s.seq.store(dequeue_pos_ + cap_, std::memory_order_release);
        ++dequeue_pos_;

        f(std::move(tmp));
        return true;
    }

    template <class F>
    std::size_t consume_up_to(F&& f, const std::size_t max_items) noexcept(noexcept(f(std::declval<T&&>())))
    {
        std::size_t n = 0;
        while (n < max_items && try_consume_one(f))
            ++n;
        return n;
    }

private:
    const std::size_t cap_;
    const std::size_t mask_;
    std::unique_ptr<Slot[]> slots_;

    alignas(std::hardware_destructive_interference_size)
        std::atomic<std::size_t> enqueue_pos_{0};

    alignas(std::hardware_destructive_interference_size)
        std::size_t dequeue_pos_{0}; // consumer-only
};
