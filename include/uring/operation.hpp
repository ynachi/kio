#pragma once
#include <coroutine>
#include <cstdint>
#include <deque>
#include <limits>

namespace URing
{
enum class SlotStatus : uint8_t
{
    free = 0,
    active,
    cancelled,
};

struct PendingOp
{
    std::coroutine_handle<> handle = nullptr;
    uint32_t generation = 0;
    int result_code = 0;
    union
    {
        uint64_t original_ud{};
        uint32_t next_free_idx;
    };
    SlotStatus status = SlotStatus::free;

    bool cancel() noexcept
    {
        if (status != SlotStatus::active)
        {
            return false;
        }
        status = SlotStatus::cancelled;
        return true;
    }
};

// Packs into io_uring's 64-bit user_data exactly.
struct Token
{
    static constexpr std::uint32_t kMaxGeneration = (1u << 30) - 1;

    std::uint32_t idx;
    std::uint32_t gen;

    [[nodiscard]] constexpr std::uint64_t pack() const noexcept
    {
        return static_cast<std::uint64_t>(gen & kMaxGeneration) << 32 | idx;
    }

    [[nodiscard]] static constexpr Token unpack(const std::uint64_t ud) noexcept
    {
        return {static_cast<std::uint32_t>(ud & 0xFFFFFFFF),
                static_cast<std::uint32_t>((ud >> 32) & kMaxGeneration)};
    }
};

class OpPool
{
    // Sentinel value indicating the free list is empty
    static constexpr uint32_t END_OF_LIST = std::numeric_limits<uint32_t>::max();

    std::deque<PendingOp> entries_;
    uint32_t head_free_idx_{END_OF_LIST};
    uint32_t next_gen_{1};

public:
    explicit OpPool(const std::size_t pool_size) noexcept
    {
        entries_.resize(pool_size);
        // Thread the initial items into the intrusive free list (LIFO order)
        for (uint32_t i = 0; i < pool_size; ++i)
        {
            entries_[i].status = SlotStatus::free;
            entries_[i].next_free_idx = head_free_idx_;
            head_free_idx_ = i;
        }
    }

    [[nodiscard]] Token allocate(std::coroutine_handle<> h) noexcept
    {
        uint32_t idx = 0;

        if (head_free_idx_ == END_OF_LIST)
        {
            // Free list is empty. We must expand the pool.
            idx = static_cast<uint32_t>(entries_.size());
            entries_.emplace_back();
        }
        else
        {
            // Pop the first item off the free list.
            idx = head_free_idx_;
            // The slot we are about to use tells us where the *next* free slot is.
            head_free_idx_ = entries_[idx].next_free_idx;
        }

        auto& op = entries_[idx];
        op.handle = h;
        op.generation = next_gen_++;
        if (next_gen_ == 0 || next_gen_ > Token::kMaxGeneration)
        {
            next_gen_ = 1;
        }

        op.result_code = 0;
        op.original_ud = 0;
        op.status = SlotStatus::active;

        return Token{idx, op.generation};
    }

    void deallocate(const Token token) noexcept
    {
        auto& op = entries_[token.idx];

        // Clean up active state
        op.handle = nullptr;
        op.status = SlotStatus::free;

        // Push this slot onto the FRONT of the free list
        op.next_free_idx = head_free_idx_;
        head_free_idx_ = token.idx;
    }

    [[nodiscard]] PendingOp* try_get(const Token token) noexcept
    {
        if (token.idx >= entries_.size())
        {
            return nullptr;
        }

        auto& op = entries_[token.idx];

        if (op.generation != token.gen || op.status == SlotStatus::free)
        {
            return nullptr;
        }

        return &op;
    }

    [[nodiscard]] PendingOp& get(const uint32_t idx) noexcept { return entries_[idx]; }

    void destroy_active_handles() noexcept
    {
        for (auto& op : entries_)
        {
            if (op.status == SlotStatus::free)
            {
                continue;
            }

            auto handle = op.handle;
            op.handle = nullptr;
            op.status = SlotStatus::free;
            op.original_ud = 0;

            if (handle)
            {
                handle.destroy();
            }
        }
    }
};

}  // namespace URing
