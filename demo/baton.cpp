//
// AsyncBaton Demo - Demonstrating cross-thread coordination patterns
//

#include "kio/sync/baton.h"

#include <chrono>
#include <iostream>
#include <thread>

#include "kio/core/coro.h"
#include "kio/core/worker.h"

using namespace kio;
using namespace kio::io;
using namespace kio::sync;

//
// Big warning, Baton syncs events not data!
//

// ============================================================================
// Demo 1: Basic Producer-Consumer Pattern
// ============================================================================
void demo_producer_consumer()
{
    std::cout << "\n=== Demo 1: Producer-Consumer ===\n";

    Worker worker(0, WorkerConfig{});
    std::jthread worker_thread([&] { worker.loop_forever(); });
    worker.wait_ready();

    AsyncBaton data_ready(worker);
    AsyncBaton consumer_done(worker);
    std::string shared_data;

    // Consumer coroutine - waits for data
    auto consumer = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);

        std::cout << "Consumer: Waiting for data...\n";
        co_await data_ready.wait();

        std::cout << "Consumer: Received data: '" << shared_data << "'\n";
        consumer_done.notify();
    };

    consumer().detach();

    // Producer thread - produces data and signals
    std::jthread producer(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                shared_data = "Hello from producer!";
                std::cout << "Producer: Data ready, notifying...\n";
                data_ready.notify();
            });

    producer.join();

    // Wait for consumer to finish before cleaning up
    while (!consumer_done.ready())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    (void) worker.request_stop();
}

// ============================================================================
// Demo 2: Request-Response with Timeout
// ============================================================================
void demo_request_response_timeout()
{
    std::cout << "\n=== Demo 2: Request-Response with Timeout ===\n";

    Worker worker(0, WorkerConfig{});
    std::jthread worker_thread([&] { worker.loop_forever(); });
    worker.wait_ready();

    AsyncBaton response_ready(worker);
    AsyncBaton request_done(worker);
    std::optional<std::string> response;

    // Requester coroutine
    auto requester = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);

        std::cout << "Requester: Sending request...\n";

        // Wait up to 200ms for response
        bool received = co_await response_ready.wait_for(std::chrono::milliseconds(200));

        if (received)
        {
            std::cout << "Requester: Got response: '" << response.value() << "'\n";
        }
        else
        {
            std::cout << "Requester: Timeout! No response received.\n";
        }

        request_done.notify();
    };

    // Fast response scenario
    std::cout << "\n-- Fast Response --\n";
    std::jthread responder(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                response = "Quick reply!";
                response_ready.notify();
            });

    requester().detach();
    responder.join();

    while (!request_done.ready())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Reset for next test
    response_ready.reset();
    request_done.reset();
    response.reset();

    // Slow response scenario (will timeout)
    std::cout << "\n-- Slow Response (Timeout) --\n";
    std::jthread slow_responder(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                response = "Too late!";
                response_ready.notify();
            });

    requester().detach();

    while (!request_done.ready())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    slow_responder.join();

    (void) worker.request_stop();
}

// ============================================================================
// Demo 3: Multi-Stage Pipeline
//
// IMPORTANT: AsyncBaton synchronizes NOTIFICATION, not DATA ACCESS!
// The 'data' variable must be atomic or protected by other means since
// multiple threads read/write it. The baton only ensures stages run in order.
// ============================================================================
void demo_pipeline()
{
    std::cout << "\n=== Demo 3: Multi-Stage Pipeline ===\n";

    Worker worker(0, WorkerConfig{});
    std::jthread worker_thread([&] { worker.loop_forever(); });
    worker.wait_ready();

    AsyncBaton stage1_done(worker);
    AsyncBaton stage2_done(worker);
    AsyncBaton stage3_done(worker);
    AsyncBaton pipeline_done(worker);

    std::atomic<int> data{0};  // Use atomic to prevent data races

    // Pipeline coordinator
    auto pipeline = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);

        std::cout << "Pipeline: Starting...\n";
        data.store(10);

        // Stage 1
        std::cout << "Pipeline: Stage 1 processing (data = " << data.load() << ")...\n";
        co_await stage1_done.wait();
        std::cout << "Pipeline: Stage 1 complete (data = " << data.load() << ")\n";

        stage1_done.reset();

        // Stage 2
        std::cout << "Pipeline: Stage 2 processing (data = " << data.load() << ")...\n";
        co_await stage2_done.wait();
        std::cout << "Pipeline: Stage 2 complete (data = " << data.load() << ")\n";

        stage2_done.reset();

        // Stage 3
        std::cout << "Pipeline: Stage 3 processing (data = " << data.load() << ")...\n";
        co_await stage3_done.wait();
        std::cout << "Pipeline: Stage 3 complete (data = " << data.load() << ")\n";

        std::cout << "Pipeline: Final result = " << data.load() << "\n";
        pipeline_done.notify();
    };

    pipeline().detach();

    // Worker threads for each stage
    std::jthread stage1(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                data.store(data.load() * 2);  // 10 -> 20 (atomic operation)
                stage1_done.notify();
            });

    std::jthread stage2(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                data.store(data.load() + 5);  // 20 -> 25 (atomic operation)
                stage2_done.notify();
            });

    std::jthread stage3(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                data.store(data.load() * 3);  // 25 -> 75 (atomic operation)
                stage3_done.notify();
            });

    stage1.join();
    stage2.join();
    stage3.join();

    while (!pipeline_done.ready())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    (void) worker.request_stop();
}

// ============================================================================
// Demo 4: Async Event Handler with Debouncing
// ============================================================================
void demo_event_debouncing()
{
    std::cout << "\n=== Demo 4: Event Debouncing ===\n";

    Worker worker(0, WorkerConfig{});
    std::jthread worker_thread([&] { worker.loop_forever(); });
    worker.wait_ready();

    AsyncBaton event_signal(worker);
    AsyncBaton handler_done(worker);
    std::atomic_int event_count{0};
    std::atomic_bool keep_running{true};

    // Event handler with debouncing
    auto event_handler = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);

        while (keep_running.load())
        {
            // Wait for event with timeout (debounce period)
            bool got_event = co_await event_signal.wait_for(std::chrono::milliseconds(100));

            if (got_event)
            {
                // Process batched events
                int count = event_count.exchange(0);
                std::cout << "Handler: Processing " << count << " batched events\n";
                event_signal.reset();
            }
            else
            {
                // Timeout - check if we should continue
                if (event_count.load() == 0 && !keep_running.load())
                {
                    break;
                }
            }
        }

        std::cout << "Handler: Shutting down\n";
        handler_done.notify();
    };

    event_handler().detach();

    // Event generator - rapid events
    std::jthread generator(
            [&]
            {
                for (int i = 0; i < 10; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    event_count.fetch_add(1);
                    event_signal.notify();
                    std::cout << "Generator: Event #" << (i + 1) << "\n";
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                keep_running.store(false);
                event_signal.notify();  // Wake up handler to exit
            });

    generator.join();

    while (!handler_done.ready())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    (void) worker.request_stop();
}

// ============================================================================
// Demo 5: Async Barrier Pattern
// ============================================================================
void demo_barrier()
{
    std::cout << "\n=== Demo 5: Async Barrier (3 Workers) ===\n";

    Worker worker(0, WorkerConfig{});
    std::jthread worker_thread([&] { worker.loop_forever(); });
    worker.wait_ready();

    AsyncBaton worker1_ready(worker);
    AsyncBaton worker2_ready(worker);
    AsyncBaton worker3_ready(worker);
    AsyncBaton all_ready(worker);

    // Coordinator waits for all workers
    auto coordinator = [&]() -> DetachedTask
    {
        co_await SwitchToWorker(worker);

        std::cout << "Coordinator: Waiting for all workers...\n";

        co_await worker1_ready.wait();
        std::cout << "Coordinator: Worker 1 ready\n";

        co_await worker2_ready.wait();
        std::cout << "Coordinator: Worker 2 ready\n";

        co_await worker3_ready.wait();
        std::cout << "Coordinator: Worker 3 ready\n";

        std::cout << "Coordinator: All workers ready! Starting main task...\n";
        all_ready.notify();
    };

    coordinator().detach();

    // Three worker threads with different startup times
    std::jthread w1(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::cout << "Worker 1: Initialization complete\n";
                worker1_ready.notify();

                // Wait for all ready signal
                while (!all_ready.ready())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                std::cout << "Worker 1: Starting work!\n";
            });

    std::jthread w2(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::cout << "Worker 2: Initialization complete\n";
                worker2_ready.notify();

                while (!all_ready.ready())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                std::cout << "Worker 2: Starting work!\n";
            });

    std::jthread w3(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                std::cout << "Worker 3: Initialization complete\n";
                worker3_ready.notify();

                while (!all_ready.ready())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                std::cout << "Worker 3: Starting work!\n";
            });

    w1.join();
    w2.join();
    w3.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    (void) worker.request_stop();
}

// ============================================================================
// Main
// ============================================================================
int main()
{
    std::cout << "AsyncBaton Usage Demos\n";
    std::cout << "======================\n";

    demo_producer_consumer();
    demo_request_response_timeout();
    demo_pipeline();
    demo_event_debouncing();
    demo_barrier();

    std::cout << "\n=== All Demos Complete ===\n";
    return 0;
}
