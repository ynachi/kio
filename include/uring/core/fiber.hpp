#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <new>

#include <sys/mman.h>

#include <boost/context/detail/fcontext.hpp>

#include "uring/error.hpp"

namespace URing
{

class IO;
class FiberIO;

/// Default fiber stack size.  Enough for a typical I/O handler with a small
/// parse buffer; increase for handlers that use large local arrays or call
/// deeply into unknown third-party code.
inline constexpr size_t kDefaultFiberStack = 64 * 1024;

/// Size of the guard page placed below every fiber stack.
/// A write into this region triggers SIGSEGV instead of silent heap corruption.
inline constexpr size_t kFiberGuardPageSize = 4096;

// ============================================================================
// FiberContext — execution state of one stackful fiber
//
// Owned by IO::owned_fibers_. Its address is stable for the fiber's lifetime;
// do not store in containers that can relocate (use std::list, not vector).
//
// Stack layout (addresses increase upward, stack grows downward):
//
//   [ guard page — PROT_NONE — kFiberGuardPageSize bytes ]
//   [ usable stack — PROT_READ|WRITE — stack_size bytes  ]  ← sp starts here (top)
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
    void*                                 stack_mem{nullptr};
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

    // Intrusive link for FiberQueue (Vyukov MPSC).
    std::atomic<FiberContext*> next_queued{nullptr};

    // Sentinel-node constructor: no stack allocation.
    // Used exclusively by FiberQueue for its internal stub node.
    struct StubTag {};
    explicit FiberContext(StubTag) noexcept {}

    explicit FiberContext(const size_t sz) : stack_size(sz)
    {
        // Allocate guard page + usable stack in one mmap call.
        // The guard page sits at the bottom (lowest address); a write into it
        // from an overflowing stack produces SIGSEGV instead of silent corruption.
        void* mem = mmap(nullptr, kFiberGuardPageSize + sz,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                         -1, 0);
        if (mem == MAP_FAILED) [[unlikely]]
            throw std::bad_alloc{};

        if (mprotect(mem, kFiberGuardPageSize, PROT_NONE) != 0) [[unlikely]]
        {
            munmap(mem, kFiberGuardPageSize + sz);
            throw std::bad_alloc{};
        }

        stack_mem = mem;
    }

    ~FiberContext()
    {
        if (stack_mem != nullptr)
            munmap(stack_mem, kFiberGuardPageSize + stack_size);
    }

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
