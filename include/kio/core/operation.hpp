#pragma once

#include "kio/logger.hpp"

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace kio
{
struct UringBackend;
struct MemoryBackend;
template <typename Backend>
class BasicIoContext;
using IoContext = BasicIoContext<UringBackend>;
using MemoryIoContext = BasicIoContext<MemoryBackend>;

enum class OpCancelReason : uint8_t
{
    None,
    Timeout,
    ExplicitCancel,
    ContextShutdown,
};

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
    void* ctx = nullptr;
    int32_t res = 0;
    std::coroutine_handle<> handle;

    // Intrusive doubly linked list pointers for internal tracking
    OperationState* next = nullptr;
    OperationState* prev = nullptr;

    // Intrusive list pointer for lock-free external submission
    std::atomic<OperationState*> next_ext{nullptr};

    bool tracked = false;
    OpCancelReason cancel_reason = OpCancelReason::None;

    OperationState() = default;

    OperationState(OperationState&& other) noexcept
        : ctx(other.ctx), res(other.res), handle(other.handle), cancel_reason(other.cancel_reason)
    {
        if (other.tracked)
        {
            ALOG_ERROR(
                "[kio] FATAL: Attempted to move an OperationState that is currently tracked by "
                "IoContext.\n[kio]        This usually means a Task was moved while suspended on I/O.");
            std::terminate();
        }
        other.ctx = nullptr;
        other.res = 0;
        other.handle = nullptr;
        other.next = nullptr;
        other.prev = nullptr;
        other.next_ext.store(nullptr, std::memory_order_relaxed);
        other.tracked = false;
        other.cancel_reason = OpCancelReason::None;
    }

    OperationState& operator=(OperationState&&) = delete;
    OperationState(const OperationState&) = delete;
    OperationState& operator=(const OperationState&) = delete;

    ~OperationState()
    {
        if (tracked == true)
        {
            ALOG_ERROR(
                "[kio] FATAL: OperationState destroyed while still tracked by IoContext (I/O pending).\n[kio] "
                "       CAUSE: A Task was destroyed while suspended on an async operation.\n[kio]        FIX: "
                "  Ensure the Task is kept alive (e.g., in a TaskGroup) until it completes.");
            std::terminate();
        }
    }
};
}  // namespace kio
