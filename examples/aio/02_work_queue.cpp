#include "aio/aio.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <future>
#include <optional>

using namespace std::chrono_literals;

// -----------------------------------------------------------------------------
// SPSC Queue Simulation (Using mutex for simplicity in demo)
// -----------------------------------------------------------------------------
struct WorkQueue {
    std::queue<int> q;
    std::mutex mtx;

    void push(int val) {
        std::lock_guard lk(mtx);
        q.push(val);
    }

    bool pop(int& val) {
        std::lock_guard lk(mtx);
        if (q.empty()) return false;
        val = q.front();
        q.pop();
        return true;
    }
};

// -----------------------------------------------------------------------------
// Worker Thread
// -----------------------------------------------------------------------------
void worker_thread(int id, int pipe_read_fd, WorkQueue& my_queue, std::promise<int>& ring_fd_promise) {
    // FIX: Declare task storage BEFORE ctx so it is destroyed AFTER ctx.
    // This ensures ctx destructor runs first, cancelling/untracking pending ops,
    // so the task destructor doesn't see tracked ops (which causes terminate).
    std::optional<aio::task<>> background_task;

    aio::io_context ctx;
    std::cout << "[Worker " << id << "] Started.\n";

    // Send Ring FD back to main immediately
    ring_fd_promise.set_value(ctx.ring_fd());

    std::vector<int> fds = { pipe_read_fd };
    ctx.register_files(fds);
    std::cout << "[Worker " << id << "] Files registered.\n";

    // The coroutine that does the work
    auto work = [&]() -> aio::task<> {
        char buf[128];
        std::span<std::byte> buffer{reinterpret_cast<std::byte*>(buf), sizeof(buf)};

        while (true) {
            std::cout << "[Worker " << id << "] Waiting for data on registered file (index 0)...\n";

            auto res = co_await aio::async_read_fixed(&ctx, 0, buffer, 0);

            if (!res) {
                std::cerr << "Read error: " << res.error().message() << "\n";
                break;
            }
            if (*res == 0) break; // EOF

            std::cout << "[Worker " << id << "] Read fixed: " << std::string(buf, *res);
        }
    };

    // Store task in the variable we prepared earlier
    background_task.emplace(work());
    background_task->start();

    // Reactor Loop
    ctx.run([&]() {
        int cmd;
        while (my_queue.pop(cmd)) {
            std::cout << "[Worker " << id << "] Woke up via MSG_RING! Processed command: " << cmd << "\n";
            if (cmd == -1) ctx.stop();
        }
    });
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main() {
    int fds[2];
    if (pipe(fds) < 0) return 1;

    WorkQueue wq;
    std::promise<int> p;
    auto f = p.get_future();

    // Start worker
    std::thread t(worker_thread, 1, fds[0], std::ref(wq), std::ref(p));

    // Wait for worker to be ready
    int worker_ring_fd = f.get();
    std::cout << "[Main] Got worker ring fd: " << worker_ring_fd << "\n";

    aio::io_context main_ctx;

    // FIX: Actually execute the coordinator logic
    auto coordinator = [&](int target_fd) -> aio::task<> {
        // 1. Send data via File I/O
        std::string msg = "Hello via Fixed I/O!\n";
        co_await aio::async_write(&main_ctx, fds[1],
            std::span<const std::byte>((std::byte*)msg.data(), msg.size()), 0);

        // 2. Send Command via MSG_RING
        std::cout << "[Main] Pushing command to queue...\n";
        wq.push(42);

        std::cout << "[Main] Sending MSG_RING wake...\n";
        co_await aio::async_msg_ring(&main_ctx, target_fd, 0, aio::WAKE_TAG);

        // 3. Cleanup
        std::cout << "[Main] Sending stop command...\n";
        wq.push(-1);
        co_await aio::async_msg_ring(&main_ctx, target_fd, 0, aio::WAKE_TAG);

        close(fds[1]);
        main_ctx.stop();
    };

    // Start the coordinator task and run the loop until it finishes
    auto coord_task = coordinator(worker_ring_fd);
    coord_task.start();
    main_ctx.run();
    
    t.join();
    return 0;
}