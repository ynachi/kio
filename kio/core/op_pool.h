//
// Created by Yao ACHI on 10/01/2026.
//

#ifndef KIO_CORE_OP_POOL_H
#define KIO_CORE_OP_POOL_H

#include <cassert>
#include <coroutine>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kio::io
{

/**
 * @brief Manages a pool of operation slots for io_uring completions.
 *
 * @note Thread-safety: Not thread-safe. Must only be used from the worker thread.
 * @note Implements generation counting to prevent ABA problems with stale completions.
 */
class OpPool
{
public:
    struct OpSlot
    {
        std::coroutine_handle<> handle;
        int result{-1};
        uint32_t generation{0};  // Prevents stale CQEs from corrupting reused slots

        void Reset()
        {
            handle = nullptr;
            result = -1;
        }
    };

    struct OpHandle
    {
        uint64_t id;
        OpPool* pool;

        // Default constructor for "Empty/Invalid" state
        OpHandle() : id(0), pool(nullptr) {}

        OpHandle(const uint64_t id, OpPool* pool) : id(id), pool(pool) {}

        // RAII: automatically returns slot to pool
        ~OpHandle()
        {
            if (pool != nullptr)
            {
                pool->Release(id);
            }
        }

        OpHandle(const OpHandle&) = delete;
        OpHandle& operator=(const OpHandle&) = delete;

        OpHandle(OpHandle&& other) noexcept : id(other.id), pool(std::exchange(other.pool, nullptr)) {}

        OpHandle& operator=(OpHandle&& other) noexcept
        {
            if (this != &other)
            {
                if (pool != nullptr)
                {
                    pool->Release(id);
                }
                id = other.id;
                pool = std::exchange(other.pool, nullptr);
            }
            return *this;
        }

        // Lightweight check for validity
        explicit operator bool() const { return pool != nullptr; }

        [[nodiscard]] uint64_t GetID() const { return id; }
    };

    explicit OpPool(const size_t initial_size = 1024, const size_t max_size = 1024 * 1024) : max_size_(max_size)
    {
        slots_.resize(initial_size);
        free_ids_.reserve(initial_size);
        // Initialize free list in reverse so we pop from the front (effectively)
        for (size_t i = 0; i < initial_size; ++i)
        {
            free_ids_.push_back(initial_size - 1 - i);
        }
    }

    // Acquire a slot and return a managed handle (Empty handle on failure)
    [[nodiscard]] OpHandle Acquire(std::coroutine_handle<> h)
    {
        if (free_ids_.empty())
        {
            if (!Grow())
            {
                return OpHandle{};  // Return empty/invalid handle
            }
        }

        auto const idx = static_cast<uint32_t>(free_ids_.back());
        free_ids_.pop_back();

        auto& slot = slots_[idx];
        slot.handle = h;
        slot.result = -1;  // Reset result specifically for new op

        // Encode generation in upper 32 bits, index in lower 32 bits
        uint64_t const id = (static_cast<uint64_t>(slot.generation) << 32) | idx;
        return OpHandle{id, this};
    }

    // Get slot by ID (validates generation)
    // Returns nullptr if the ID is stale (generation mismatch)
    [[nodiscard]] OpSlot* Get(uint64_t id)
    {
        auto const idx = static_cast<uint32_t>(id);
        auto const gen = static_cast<uint32_t>(id >> 32);

        if (idx >= slots_.size())
        {
            return nullptr;
        }

        auto& slot = slots_[idx];
        if (slot.generation != gen)
        {
            return nullptr;
        }

        return &slot;
    }

    // Stats
    [[nodiscard]] size_t Size() const { return slots_.size(); }
    [[nodiscard]] size_t Active() const { return slots_.size() - free_ids_.size(); }
    [[nodiscard]] size_t Capacity() const { return slots_.size(); }

private:
    friend struct OpHandle;

    void Release(const uint64_t id)
    {
        auto const idx = static_cast<uint32_t>(id);
        auto const gen = static_cast<uint32_t>(id >> 32);

        if (idx < slots_.size())
        {
            // Only release if generation matches (sanity check)
            if (auto& slot = slots_[idx]; slot.generation == gen)
            {
                slot.Reset();
                slot.generation++;  // Increment generation to invalidate old IDs
                free_ids_.push_back(idx);
            }
        }
    }

    // Acquire an op slot (may grow pool)
    bool Grow()
    {
        const size_t current = slots_.size();
        if (current >= max_size_)
        {
            return false;
        }

        size_t const new_size = std::min(static_cast<size_t>(current * 1.5), max_size_);
        if (new_size <= current)
        {
            return false;
        }

        size_t const additional = new_size - current;

        try
        {
            slots_.resize(new_size);

            size_t const old_free_size = free_ids_.size();
            free_ids_.resize(old_free_size + additional);

            for (size_t i = 0; i < additional; ++i)
            {
                // Add new indices to free list
                free_ids_[old_free_size + i] = (new_size - 1) - i;
            }
        }
        catch (const std::bad_alloc&)
        {
            return false;  // OS refused to give more memory
        }

        return true;
    }

    std::vector<OpSlot> slots_;
    std::vector<uint64_t> free_ids_;
    size_t max_size_;
};

}  // namespace kio::io

#endif  // KIO_CORE_OP_POOL_H