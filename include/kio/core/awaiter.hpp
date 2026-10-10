#pragma once

// ============================================================================
// kio::IoAwaiter -- awaitable adapter for a single io_uring operation
// ============================================================================
//
// This is the bridge between C++20 coroutines and io_uring. One IoAwaiter value
// == one SQE == one CQE == one resumption of the awaiting coroutine.
//
// THE CENTRAL INVARIANT (I2): THE FRAME IS THE OPERATION TABLE
// ----------------------------------------------------------
//
//   IoOps is a MEMBER of IoAwaiter, and IoAwaiter is a member of the
//   coroutine frame that is awaiting the operation. The SQE's user_data is set
//   to &ops_, i.e. a pointer INTO that live frame:
//
//        coroutine frame                     kernel
//        +---------------------------+      +------------------+
//        | ... coroutine locals ...  |      |  CQE.user_data --+-----> &ops_
//        | IoAwaiter:                |      |  CQE.res         |      |
//        |   IoOps  ops_   <----------------+  (filled in)      |      |
//        |   setup_ / mapper_        |                         |      |
//        +---------------------------+                         +------------------+
//
//   Therefore, when the kernel posts the CQE, &ops_ is guaranteed to point at
//   storage that is alive, because the only way this coroutine's frame dies is
//   by destroying the frame -- and destroying the frame means the coroutine is
//   no longer awaiting anything, so no CQE can be in flight for it.
//
//   CONSEQUENCE: there is no in-flight registry, no operation table, no
//   cancellation map, no ABA hazard, and no per-operation refcount anywhere in
//   kio. All of those were designs that were tried and rejected precisely
//   because a registry duplicates a guarantee the language already gives us.
//   The hot path is therefore just "write ops_.res when the CQE arrives".
//
//   What this invariant does NOT license: destroying a frame that still has an
//   outstanding SQE. That is what IO's drain logic in core/io.h enforces -- the
//   worker will not free frames until the ring reports no operation in flight
//   for them. Keep these two halves in mind together; each assumes the other.
//
// INVARIANT: IoAwaiter is IMMOVABLE and IMMUNCOPYABLE (enforced by the deleted
// special members below). Its address is published to the kernel via &ops_, so
// the compiler is forbidden from producing a different address. This is why the
// I/O helpers return by value and rely on C++17 mandatory copy elision to keep
// the object in the frame.
// ============================================================================

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <liburing.h>

#include "kio/error.hpp"

namespace kio
{
    class IO;

    // ------------------------------------------------------------------------
    // IoOps -- the kernel-visible half of an operation
    // ------------------------------------------------------------------------
    //
    // This is the entire per-operation state the kernel touches. Two fields, no
    // locks, no atomics: exactly one thread (the owning worker, per invariant
    // I1 in core/io.h) both reads and writes it, because completions are
    // consumed by the same thread that submitted them.
    //
    //   h     the coroutine to resume. Defaults to noop_coroutine() so that a
    //         CQE arriving for an operation that was never wired up cannot jump
    //         to a null handle -- resuming noop is defined and inert.
    //   res   the CQE result. The kernel's convention is NEGATIVE errno on
    //         failure and >= 0 on success; the MapperFunc translates it into
    //         Result<T> at await_resume() time.
    struct IoOps
    {
        std::coroutine_handle<> h{std::noop_coroutine()};
        int32_t res{-1};
    };

    // ------------------------------------------------------------------------
    // IoAwaiter<SetupFunc, MapperFunc>
    // ------------------------------------------------------------------------
    //
    // Two callables, both stored by value:
    //
    //   SetupFunc  fills an io_uring_sqe*. Signature: void(io_uring_sqe*).
    //             This is where the operation is actually described -- which
    //             opcode, which fd, which buffers, which flags.
    //   MapperFunc converts int32_t (the raw CQE result) into Result<T>.
    //             Signature: Result<T>(int32_t). This is the only place the
    //             negative-errno convention is interpreted, so it can differ
    //             per operation: a read wants a byte count, a close wants a
    //             bare success, an accept wants an fd.
    //
    // Both are template parameters constrained by concepts, so neither is
    // type-erased at runtime -- no std::function, no allocation, and the
    // mapper inlines into await_resume (which is why a captureless lambda
    // costs nothing on the resume path).
    template <typename SetupFunc, typename MapperFunc>
        requires std::invocable<SetupFunc, io_uring_sqe*> && std::invocable<MapperFunc, int32_t>
    class IoAwaiter
    {
        // Extract the exact Result<T> type returned by the MapperFunc
        using ResultType = std::invoke_result_t<MapperFunc, int32_t>;
        // Extract the T from Result<T> so we can type the coroutine handle
        using T = ResultType::value_type;

        // for friend access
        //
        // THIS is the object the kernel writes into, and the address handed to
        // io_uring_sqe_set_data(). See I2 above -- its lifetime is the frame's
        // lifetime, by construction.
        IoOps ops_{};

        // Reference to the owning IO. Stored as a reference rather than a
        // pointer so that a null IO is unrepresentable at the type level.
        IO& io_;

        // [[no_unique_address]] so an empty (captureless) lambda costs zero
        // bytes. Most operations in this library have stateless setups, which
        // makes the whole awaiter roughly {ops, pointer} in size.
        [[no_unique_address]] SetupFunc setup_;
        [[no_unique_address]] MapperFunc mapper_;

    public:
        IoAwaiter(IO& io, SetupFunc setup, MapperFunc mapper)
            : io_(io), setup_(std::move(setup)), mapper_(std::move(mapper))
        {
        }

        // Always false: an io_uring operation always has to go to the kernel,
        // so there is never an "already complete, do not suspend" fast path.
        // Returning true here would silently skip the submission entirely.
        bool await_ready() const noexcept { return false; }

        // IoAwaiter MUST be pinned in the coroutine frame because it captures
        // pointers/references to buffers and metadata that the kernel will
        // access asynchronously. Deleting the move constructor ensures stability
        // and relies on C++17 mandatory copy elision for RVO from I/O functions.
        //
        // INVARIANT: the object whose address reaches the kernel is the one
        // inside the frame. Copying or moving it would create a second object
        // whose address differs from &ops_ in the SQE, and the kernel would then
        // write into the abandoned copy.
        IoAwaiter(IoAwaiter&& other) noexcept = delete;
        IoAwaiter& operator=(IoAwaiter&& other) = delete;
        IoAwaiter(const IoAwaiter&) = delete;

        ~IoAwaiter() = default;

        // Defined out-of-line in core/io.h, after IO is complete: submitting
        // requires the ring, the admission gate and the SQE/completion
        // machinery, all of which need IO's full definition.
        //
        // Contract of the out-of-line definition:
        //   * fill ops_.h with the awaiting coroutine `h`
        //   * obtain an SQE from io_ and populate it via setup_(sqe)
        //   * set sqe->user_data = &ops_
        //   * publish to the ring
        //   * ALWAYS return std::noop_coroutine() (i.e. suspend), even when the
        //     submission itself failed -- the failure is recorded in ops_.res
        //     so that the resume path has exactly one shape and await_resume
        //     stays total.
        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept;

        // Runs on the owning worker after the CQE was harvested and ops_.res
        // was written. Pure computation: maps the raw result, touches no
        // global state. Must be [[nodiscard]] upstream, because a dropped
        // Result<T> silently swallows an I/O error.
        Result<T> await_resume() noexcept { return mapper_(ops_.res); }
    };

    // Implementation is moved to context.h after IO is fully defined

    // Helper for void-returning operations in io.hpp (e.g., detail::ResumeVoid)
    namespace detail
    {
        // Maps the CQE result for operations that produce no value.
        //
        // Sign convention: io_uring reports errors as -errno, so the errno is
        // recovered by negating. Anything >= 0 (for these operations, 0) is
        // success. ECANCELED is what an operation receives when teardown
        // cancels it, and it surfaces here as a normal error rather than being
        // swallowed -- see IO::drain_until_quiescent() in core/io.h.
        struct ResumeVoid
        {
            Result<void> operator()(const int32_t res) const noexcept
            {
                if (res < 0)
                {
                    return Error::fail_errno(-res);
                }

                return {};
            }
        };

        // Maps the CQE result for operations that yield a 32-bit value (read,
        // write, accept). On success the byte count or fd is returned as-is;
        // note the raw kernel value is NOT negated on the success path.
        struct ResumeInt
        {
            Result<int32_t> operator()(const int32_t res) const noexcept
            {
                if (res < 0)
                {
                    return Error::fail_errno(-res);
                }
                return res;
            }
        };
    } // namespace detail
} // namespace kio
