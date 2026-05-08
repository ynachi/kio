#pragma once
#include <coroutine>
#include <cstdint>
#include <cstddef>
#include <memory_resource>

namespace URing
{

// ---------------------------------------------------------
// Thread-Local PMR for Coroutine Frames
// ---------------------------------------------------------
// 8mb slab
// Coroutine frames are typically 512B - 4KB, so 1MB can hold ~256-2048 * 8 concurrent frames.
inline thread_local std::byte tl_coro_buf[1024 * 1024 * 8];
inline thread_local std::pmr::monotonic_buffer_resource tl_mono{tl_coro_buf, sizeof(tl_coro_buf)};
inline thread_local std::pmr::unsynchronized_pool_resource tl_coro_pool{&tl_mono};

// ---------------------------------------------------------
// The Generational Token
// ---------------------------------------------------------
/// Packs exactly into io_uring's 64-bit user_data.
struct Token
{
    std::uint32_t idx;
    std::uint32_t gen;

    [[nodiscard]]
    std::uint64_t to_u64() const noexcept
    {
        return static_cast<std::uint64_t>(gen) << 32 | static_cast<std::uint64_t>(idx);
    }

    static Token from_u64(const std::uint64_t val) noexcept
    {
        return {static_cast<std::uint32_t>(val & 0xFFFFFFFF), static_cast<std::uint32_t>(val >> 32)};
    }
};

// ---------------------------------------------------------
// The OpState (The Intrusive Free List Node)
// ---------------------------------------------------------
struct OpState
{
    std::uint32_t gen = 0;
    std::uint32_t next_free_idx = 0;

    std::coroutine_handle<> coro_handle = nullptr;
    std::int32_t cqe_res = 0;
    // The Zombie flag
    bool is_abandoned = false;
    bool is_in_use = false;
    bool cancel_requested = false;
};

}  // namespace URing
