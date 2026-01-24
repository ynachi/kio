#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <print>
#include <thread>

#include "aio/aio.hpp"

// examples/msg_ring_wakeup.cpp
// Demonstrates: Correctly waking a SINGLE_ISSUER ring from another thread.
//
// SAFETY NOTE:
// When sharing a pointer to a stack-allocated IoContext (remote_ctx),
// you must ensure the worker thread does not destroy the context while
// the main thread is still trying to use it (call Notify).
// We use a promise/future barrier to ensure safe destruction.

int main()
{
    std::atomic<bool> stop{false};

    std::promise<aio::IoContext*> ctx_promise;
    auto ctx_future = ctx_promise.get_future();

    // Barrier to prevent Use-After-Free
    std::promise<void> shutdown_barrier;
    auto shutdown_complete = shutdown_barrier.get_future();

    std::thread worker(
        [&]
        {
            // 1. Create Context ON THE THREAD that will run the loop
            aio::IoContext worker_ctx(256);

            worker_ctx.WaitReady();

            // 2. Expose the pointer to the main thread
            ctx_promise.set_value(&worker_ctx);

            std::println(stderr, "Worker loop starting...");

            // 3. Run the loop
            worker_ctx.Run(
                [&]
                {
                    if (stop.load(std::memory_order_relaxed))
                        worker_ctx.Stop();
                });

            // 4. WAIT for main thread to finish calling Notify()
            // If we destroy worker_ctx now, the main thread's Notify() call
            // might race with the destructor (close(wake_fd)).
            shutdown_complete.wait();
            worker_ctx.CancelAllPending();
            std::println(stderr, "Worker exited");
        });

    // Main thread waits until worker has created the context
    aio::IoContext* remote_ctx = ctx_future.get();

    std::println(stderr, "Main thread sleeping...");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::println(stderr, "Main thread sending wake signal...");

    // 5. Signal the worker to stop
    stop.store(true, std::memory_order_relaxed);

    // 6. ERGONOMIC WAKE
    if (!remote_ctx->Notify())
    {
        std::println(stderr, "Failed to notify worker!");
    }

    // 7. Signal worker it's safe to destroy the context
    shutdown_barrier.set_value();

    worker.join();
    return 0;
}