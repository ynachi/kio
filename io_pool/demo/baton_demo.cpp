////////////////////////////////////////////////////////////////////////////////
// baton_demo.cpp - Usage examples for AsyncBaton
//
// Demonstrates:
// 1. Coroutine timeout logic (wait_for)
// 2. Producer-Consumer signaling
// 3. Waking up a blocking thread from a coroutine
////////////////////////////////////////////////////////////////////////////////

#include <memory>

#include "../kio_logger.hpp"
#include "io_pool/runtime.hpp"
#include "utilities/baton.hpp"

using namespace uring;

// -----------------------------------------------------------------------------
// Demo 1: wait_for (Success Case)
// The producer signals AFTER 100ms, and we wait for 500ms.
// Expected: Returns true (Success)
// -----------------------------------------------------------------------------
Task<> demo_success_case(ThreadContext& ctx) {
    // Shared ownership because multiple tasks access it
    auto baton = std::make_shared<AsyncBaton>();

    Log::info("[Demo 1] Starting: Waiting 500ms for signal...");

    // Spawn a "Producer" task that works for 100ms then signals
    ctx.executor().spawn([baton, &ctx]() -> Task<> {
        co_await timeout_ms(ctx.executor(), 100);
        Log::info("[Demo 1] Producer: Work done. Posting!");
        baton->post();
    }());

    // Consumer waits
    bool posted = co_await baton->wait_for(ctx, std::chrono::milliseconds(500));

    if (posted) {
        Log::info("[Demo 1] SUCCESS: Signal received in time.");
    } else {
        Log::error("[Demo 1] FAIL: Timed out unexpectedly.");
    }
}

// -----------------------------------------------------------------------------
// Demo 2: wait_for (Timeout Case)
// The producer signals AFTER 500ms, but we only wait 100ms.
// Expected: Returns false (Timeout)
// -----------------------------------------------------------------------------
Task<> demo_timeout_case(ThreadContext& ctx) {
    auto baton = std::make_shared<AsyncBaton>();

    Log::info("[Demo 2] Starting: Waiting 100ms for signal...");

    // Producer is too slow...
    ctx.executor().spawn([baton, &ctx]() -> Task<> {
        co_await timeout_ms(ctx.executor(), 500);
        // Note: By the time this runs, the consumer will have already moved on.
        // The baton handles this race condition safely.
        Log::debug("[Demo 2] Producer: Posting late signal (ignored by waiter)");
        baton->post();
    }());

    bool posted = co_await baton->wait_for(ctx, std::chrono::milliseconds(100));

    if (!posted) {
        Log::info("[Demo 2] SUCCESS: Operation timed out as expected.");
    } else {
        Log::error("[Demo 2] FAIL: Got signal unexpectedly.");
    }
}

// -----------------------------------------------------------------------------
// Demo 3: Thread Blocking
// Main thread blocks, Runtime thread wakes it up.
// -----------------------------------------------------------------------------
void demo_thread_blocking(Runtime& rt) {
    AsyncBaton baton;
    Log::info("[Demo 3] Main Thread: Blocking wait...");

    // Schedule a task on the runtime to wake us up
    rt.schedule([&]() -> Task<> {
        co_await timeout_ms(rt.thread(0).executor(), 200);
        Log::info("[Demo 3] Runtime: Posting signal to main thread!");
        baton.post();
    });

    // Block the main thread entirely (no CPU usage)
    baton.wait();

    Log::info("[Demo 3] Main Thread: Woke up!");
}

int main() {
    Runtime rt;
    rt.loop_forever();

    Log::info("=== Running AsyncBaton Demos ===");

    // We run the coroutine demos on Thread 0
    auto& ctx = rt.thread(0);

    // Run sequentially for clarity
    block_on(ctx, demo_success_case(ctx));
    block_on(ctx, demo_timeout_case(ctx));

    demo_thread_blocking(rt);

    rt.stop();
    Log::info("All demos completed.");
    return 0;
}