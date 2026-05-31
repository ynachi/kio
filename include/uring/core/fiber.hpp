#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ucontext.h>

#include "uring/error.hpp"

namespace URing
{

class IO;
class FiberIO;

// ============================================================================
// FiberContext — execution state of one stackful coroutine (fiber)
//
// Owned by IO::owned_fibers_. Its address is stable for the fiber's lifetime;
// do not store in containers that can relocate (use std::list, not vector).
// ============================================================================
struct FiberContext
{
    ucontext_t                            ctx{};
    ucontext_t*                           scheduler_ctx{nullptr};  // IO::scheduler_ctx_
    std::unique_ptr<std::byte[]>          stack;
    size_t                                stack_size{0};
    bool                                  done{false};
    IO*                                   io{nullptr};
    std::function<Result<void>(FiberIO&)> fn;
    Result<void>                          result{};

    explicit FiberContext(const size_t sz)
        : stack(std::make_unique<std::byte[]>(sz)), stack_size(sz)
    {}

    // Non-movable: raw pointer to this is stored in ready_fibers_ and SQE user_data
    FiberContext(const FiberContext&)            = delete;
    FiberContext& operator=(const FiberContext&) = delete;
    FiberContext(FiberContext&&)                 = delete;
    FiberContext& operator=(FiberContext&&)      = delete;
};

// Stored in SQE user_data with bit 0 set to 1, distinguishing it from IoOps*
// (which are always even-aligned).  tick() checks bit 0 to dispatch correctly.
struct FiberOps
{
    FiberContext* fiber{nullptr};
    int32_t       res{-1};
};

namespace detail
{
// Called by makecontext. The FiberContext* is split into two 32-bit words to
// satisfy makecontext's int-arg ABI on x86-64.
void fiber_entry(uint32_t hi, uint32_t lo) noexcept;
}  // namespace detail

}  // namespace URing
