// examples/09_multi_worker.cpp
// Demonstrates: Worker, Notifier, cross-thread communication

#include <atomic>
#include <chrono>
#include <print>
#include <vector>

#include "aio/io.hpp"
#include "aio/notifier.hpp"
#include "aio/task.hpp"
#include "aio/worker.hpp"

using namespace std::chrono_literals;

struct WorkItem
{
    int id;
    int value;
};

class WorkQueue
{
public:
    void Push(WorkItem item)
    {
        {
            std::scoped_lock lock(mutex_);
            items_.push_back(item);
        }
        notifier_.Signal();
    }

    std::vector<WorkItem> DrainAll()
    {
        std::scoped_lock lock(mutex_);
        return std::exchange(items_, {});
    }

    auto Wait(aio::IoContext& ctx) { return notifier_.Wait(ctx); }

private:
    std::mutex mutex_;
    std::vector<WorkItem> items_;
    aio::Notifier notifier_;
};

aio::Task<> worker_loop(aio::IoContext& ctx, int worker_id,
                        WorkQueue& queue, std::atomic<bool>& running)
{
    std::println("[Worker {}] Started", worker_id);

    while (running.load(std::memory_order_relaxed))
    {
        auto wait_result = co_await queue.Wait(ctx).WithTimeout(100ms);

        auto items = queue.DrainAll();
        for (const auto& item : items)
        {
            // Simulate processing
            co_await aio::AsyncSleep(ctx, 50ms);
            std::println("[Worker {}] Processed item {} (value={})",
                         worker_id, item.id, item.value);
        }
    }

    std::println("[Worker {}] Stopped", worker_id);
}

int main()
{
    constexpr int num_workers = 3;

    std::atomic<bool> running{true};
    WorkQueue queue;

    // Start workers
    std::vector<aio::Worker> workers;
    workers.reserve(num_workers);

    for (int i = 0; i < num_workers; ++i)
    {
        workers.emplace_back(i);
        workers.back().RunTask(
            [i, &queue, &running](aio::IoContext& ctx) {
                return worker_loop(ctx, i, queue, running);
            });
    }

    // Producer: submit work items
    std::println("\nSubmitting work items...\n");

    for (int i = 1; i <= 10; ++i)
    {
        queue.Push({i, i * 100});
        std::this_thread::sleep_for(20ms);  // Stagger submissions
    }

    // Wait for processing
    std::this_thread::sleep_for(1s);

    // Shutdown
    std::println("\nShutting down...");
    running.store(false, std::memory_order_relaxed);

    for (auto& w : workers)
    {
        w.RequestStop();
    }
    for (auto& w : workers)
    {
        w.Join();
    }

    std::println("Done");
    return 0;
}