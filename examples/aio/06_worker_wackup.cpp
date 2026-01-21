// examples/msg_ring_wakeup.cpp
#include <atomic>
#include <chrono>
#include <cstdio>
#include <print>
#include <thread>

#include "aio/aio.hpp"

// examples/msg_ring_wakeup.cpp — wake another ring (WAKE_TAG)
// Demonstrates: IORING_OP_MSG_RING used purely as a wakeup mechanism.
// Shows why you need wakeups: run() can be sleeping in submit_and_wait().

static aio::task<> send_wake(aio::io_context& sender, int target_ring_fd) {
    (void)co_await aio::async_msg_ring(&sender, target_ring_fd, /*res*/0u, /*user_data*/aio::WAKE_TAG);
    co_return;
}

int main() {
    aio::io_context worker_ctx(256);
    std::atomic stop{false};

    std::thread worker([&] {
        // No tasks: it will often block in submit_and_wait().
        worker_ctx.run([&] {
            if (stop.load(std::memory_order_relaxed)) worker_ctx.stop();
        });
        worker_ctx.cancel_all_pending();
        std::println(stderr, "worker exited");
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Flip flag, then WAKE_TAG to force worker to return from wait and observe it.
    stop.store(true, std::memory_order_relaxed);

    aio::io_context sender_ctx(32);
    auto t = send_wake(sender_ctx, worker_ctx.ring_fd());
    sender_ctx.run_until_done(t);

    worker.join();
    return 0;
}
