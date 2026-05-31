#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
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
// ctx           — fiber's saved execution state (updated on each suspend)
// scheduler_ctx — event loop's saved state (updated on each jump to fiber)
// last_res      — CQE result written by tick() before resuming the fiber
// self_it       — iterator into owned_fibers_ for O(1) removal on completion
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
    // move_only_function: avoids CopyConstructible requirement and the extra
    // heap allocation that std::function incurs for large captures.
    std::move_only_function<Result<void>(FiberIO&)> fn;
    Result<void>                                    result{};
    // Filled by spawn_fiber after insertion; enables O(1) self-removal.
    std::list<std::unique_ptr<FiberContext>>::iterator self_it{};

    explicit FiberContext(const size_t sz)
        : stack(std::make_unique<std::byte[]>(sz)), stack_size(sz)
    {}

    // Non-movable: raw pointer to this is stored in ready_fibers_ and SQE user_data
    FiberContext(const FiberContext&)            = delete;
    FiberContext& operator=(const FiberContext&) = delete;
    FiberContext(FiberContext&&)                 = delete;
    FiberContext& operator=(FiberContext&&)      = delete;
};

namespace detail
{
// Fiber entry point for Boost.Context. Called by jump_fcontext when a fiber
// is first resumed. t.data is the FiberContext*; t.fctx is the scheduler's
// context to jump back to on suspend/completion.
void fiber_entry(boost::context::detail::transfer_t t) noexcept;
}  // namespace detail

}  // namespace URing
