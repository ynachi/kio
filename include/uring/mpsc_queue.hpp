#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <limits>
#include <utility>

namespace URing
{

// ============================================================================
// Platform & Safety Guards
// ============================================================================

// io_uring user_data is 64 bits. Storing coroutine pointers directly requires
// a 64-bit address space. This is guaranteed on all modern Linux targets.
static_assert(sizeof(void*) == 8,
              "This library requires a 64-bit platform for safe coroutine handle "
              "storage in io_uring_sqe_set_data64()");

inline constexpr std::size_t kCacheLineSize = 64;

// ============================================================================
// MpscQueue<T>
//
// Multi-producer single-consumer lock-free queue (Vyukov algorithm).
// Wait-free enqueue/dequeue. Payload T is trivially copyable.
// ============================================================================
template <typename T>
    requires std::is_trivially_copyable_v<T>
class MpscQueue
{
public:
    struct Node
    {
        std::atomic<Node*> next{nullptr};
        std::optional<T> value;

        Node() = default;
        explicit Node(T v) : value(std::move(v)) {}
    };

    // Wait-free enqueue. Synchronizes with dequeue via acq_rel/release.
    void enqueue(T item)
    {
        Node* node = new Node(std::move(item));
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    // Wait-free dequeue (single consumer only).
    std::optional<T> dequeue()
    {
        Node* next = tail_->next.load(std::memory_order_acquire);
        if (next == nullptr)
        {
            return std::nullopt;
        }

        std::optional<T> val = std::move(next->value);
        delete tail_;
        tail_ = next;
        return val;
    }

    template <typename Fn>
    std::size_t drain(Fn&& fn, const std::size_t max_count = std::numeric_limits<std::size_t>::max())
    {
        std::size_t count = 0;
        while (count < max_count)
        {
            auto val = dequeue();
            if (!val.has_value())
            {
                break;
            }
            fn(*val);
            ++count;
        }
        return count;
    }

    bool empty() const noexcept { return tail_->next.load(std::memory_order_acquire) == nullptr; }

    MpscQueue()
    {
        Node* s = new Node{};
        head_.store(s, std::memory_order_seq_cst);
        tail_ = s;
    }

    ~MpscQueue()
    {
        while (dequeue())
        {
        }
        delete tail_;
    }

private:
    alignas(kCacheLineSize) std::atomic<Node*> head_;
    alignas(kCacheLineSize) Node* tail_{nullptr};
};

}  // namespace URing
