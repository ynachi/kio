#pragma once

#include <coroutine>
#include <cstdint>
#include <exception>

namespace aio
{
class IoContext;
// -----------------------------------------------------------------------------
// Operation State (Intrusive Tracking)
// -----------------------------------------------------------------------------

/**
 * Base state for all pending I/O operations.
 *
 * Linked into io_context's pending list on submission, unlinked on completion.
 * If destroyed while still linked, the program terminates — this catches bugs
 * where a coroutine frame is destroyed while its I/O is still in flight.
 *
 * WARNING: This is detection, not prevention. The operation is embedded in the
 * coroutine frame, so destroying the task destroys the operation. The terminate()
 * is a fail-fast to avoid silent memory corruption.
 *
 * Movable only when not tracked (before await_suspend).
 */
struct OperationState
{
    IoContext* ctx = nullptr;
    int32_t res = 0;
    std::coroutine_handle<> handle;

    // Intrusive doubly linked list pointers
    OperationState* next = nullptr;
    OperationState* prev = nullptr;
    bool tracked = false;

    OperationState() = default;

    // Move allowed only when not tracked
    OperationState(OperationState&& other) noexcept : ctx(other.ctx), res(other.res), handle(other.handle)
    {
        // Source must not be tracked
        if (other.tracked)
        {
            std::terminate();
        }
        other.ctx = nullptr;
        other.handle = nullptr;
    }

    OperationState& operator=(OperationState&&) = delete;
    OperationState(const OperationState&) = delete;
    OperationState& operator=(const OperationState&) = delete;

    ~OperationState()
    {
        if (tracked == true)
        {
            // Coroutine destroyed while I/O pending → memory corruption risk
            // Terminate loudly rather than corrupt silently
            std::terminate();
        }
    }
};
}  // namespace aio