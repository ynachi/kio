#pragma once

#include <coroutine>
#include <exception>

#include "uring/logger.hpp"

namespace URing::detail
{

// -----------------------------------------------------------------------
// FinalAwaitable — The "Self-Cleaning" Mechanism
// -----------------------------------------------------------------------
struct FinalAwaitable
{
    bool await_ready() noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept
    {
        auto cont = h.promise().continuation;

        // If continuation is noop, it means this task was "detached" (scheduled).
        if (cont == std::noop_coroutine())
        {
            // CRITICAL MEMORY SAFETY: Catch and report unhandled exceptions
            // in detached tasks before destroying the coroutine frame.
            if (h.promise().exception)
            {
                try
                {
                    std::rethrow_exception(h.promise().exception);
                }
                catch (const std::exception& e)
                {
                    ALOG_FATAL("Detached task terminated with unhandled exception: {}", e.what());
                }
                catch (...)
                {
                    ALOG_FATAL("Detached task terminated with unknown unhandled exception!");
                }

                // Emulate standard thread lifecycle behavior for unhandled exceptions
                std::terminate();
            }

            h.destroy();
            return std::noop_coroutine();
        }

        // Otherwise, symmetrically transfer control back to the caller.
        return cont;
    }

    void await_resume() noexcept {}
};

// ============================================================================
// task_promise_base — Shared logic for all URing tasks
// ============================================================================
struct TaskPromiseBase
{
    std::coroutine_handle<> continuation{std::noop_coroutine()};
    std::exception_ptr exception = nullptr;

    // intrusive queue hook
    std::atomic<TaskPromiseBase*> next{nullptr};
#ifndef NDEBUG
    std::atomic<bool> is_enqueued{false};
#endif

    // The type-erased handle used by the event loop to resume this frame
    std::coroutine_handle<> self_handle{nullptr};

    std::suspend_always initial_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept { exception = std::current_exception(); }

    FinalAwaitable final_suspend() noexcept { return {}; }
};
}  // namespace URing::detail