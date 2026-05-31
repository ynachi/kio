#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <boost/context/detail/fcontext.hpp>

#include "uring/error.hpp"

namespace URing
{

class IO;
class FiberIO;

// ============================================================================
// FiberContext — execution state of one stackful fiber
//
// Owned by IO::owned_fibers_. Its address is stable for the fiber's lifetime;
// do not store in containers that can relocate (use std::list, not vector).
//
// ctx          — the fiber's saved execution state (updated on each suspend)
// scheduler_ctx — the event loop's saved state (updated on each jump to fiber)
// last_res      — CQE result written by tick() before resuming the fiber
// ============================================================================
struct FiberContext
{
    boost::context::detail::fcontext_t    ctx{nullptr};
    boost::context::detail::fcontext_t    scheduler_ctx{nullptr};
    std::unique_ptr<std::byte[]>          stack;
    size_t                                stack_size{0};
    bool                                  done{false};
    IO*                                   io{nullptr};
    int32_t                               last_res{0};
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
};

namespace detail
{
// Fiber entry point for Boost.Context. Called by jump_fcontext when a fiber
// is first resumed. t.data is the FiberContext*; t.fctx is the scheduler's
// context to jump back to on suspend/completion.
void fiber_entry(boost::context::detail::transfer_t t) noexcept;
}  // namespace detail

}  // namespace URing
