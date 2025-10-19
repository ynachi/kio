//
// Created by Yao ACHI on 19/10/2025.
//

#include "core/include/ds/mpsc_queue.h"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <iostream>
#include <thread>
#include <vector>

namespace kio
{

    struct Tracker
    {
        static inline std::atomic<int> alive{0};
        static inline std::atomic<int> constructions{0};
        static inline std::atomic<int> destructions{0};
        static inline std::atomic<int> moves{0};
        static inline std::atomic<int> copies{0};

        std::string data;

        explicit Tracker(std::string s) : data(std::move(s))
        {
            ++constructions;
            ++alive;
        }
        Tracker(const Tracker& other) : data(other.data)
        {
            ++copies;
            ++constructions;
            ++alive;
        }
        Tracker(Tracker&& other) noexcept : data(std::move(other.data))
        {
            ++moves;
            ++constructions;  // move constructs a new object
            ++alive;
        }
        Tracker& operator=(Tracker&& other) noexcept
        {
            data = std::move(other.data);
            ++moves;
            return *this;
        }
        ~Tracker()
        {
            ++destructions;
            --alive;
        }
    };


    // Test 1: Basic single-threaded operations
    void test_basic_operations()
    {
        std::cout << "Test 1: Basic operations... ";

        MPSCQueue<int> queue(16);

        // Push some items
        assert(queue.try_push(1));
        assert(queue.try_push(2));
        assert(queue.try_push(3));

        // Pop them back
        int val;
        assert(queue.try_pop(val) && val == 1);
        assert(queue.try_pop(val) && val == 2);
        assert(queue.try_pop(val) && val == 3);
        assert(!queue.try_pop(val));  // Empty

        std::cout << "PASSED\n";
    }

    // Test 2: Fill and drain
    void test_fill_and_drain()
    {
        std::cout << "Test 2: Fill and drain... ";

        MPSCQueue<int> queue(16);

        const size_t cap = queue.capacity();  // use capacity() to avoid hardcoding

        // Fill completely (capacity number of items)
        for (size_t i = 0; i < cap; ++i)
        {
            assert(queue.try_push(static_cast<int>(i)));
        }

        // Should be full now
        assert(!queue.try_push(999));

        // Drain completely
        for (size_t i = 0; i < cap; ++i)
        {
            int val;
            assert(queue.try_pop(val) && val == static_cast<int>(i));
        }

        // Should be empty
        int val;
        assert(!queue.try_pop(val));

        std::cout << "PASSED\n";
    }

    // Test 3: Multi-producer stress test
    void test_multi_producer()
    {
        std::cout << "Test 3: Multi-producer (this may take a moment)... ";

        constexpr size_t num_producers = 8;
        constexpr size_t items_per_producer = 10000;
        constexpr size_t total_items = num_producers * items_per_producer;

        MPSCQueue<size_t> queue(1024);
        std::atomic start_flag{false};
        std::atomic<size_t> push_failures{0};

        // Producer threads
        std::vector<std::thread> producers;
        producers.reserve(num_producers);
        for (size_t p = 0; p < num_producers; ++p)
        {
            producers.emplace_back(
                    [&, p]()
                    {
                        // Wait for all threads to be ready
                        while (!start_flag.load(std::memory_order_acquire))
                        {
                            std::this_thread::yield();
                        }

                        // Push items with thread-specific values
                        for (size_t i = 0; i < items_per_producer; ++i)
                        {
                            size_t value = (p << 32) | i;  // Encode thread ID and counter

                            // Retry on failure (queue temporarily full)
                            while (!queue.try_push(value))
                            {
                                push_failures.fetch_add(1, std::memory_order_relaxed);
                                std::this_thread::yield();
                            }
                        }
                    });
        }

        // Consumer thread
        std::vector<size_t> received_counts(num_producers, 0);
        std::atomic<size_t> total_received{0};

        std::thread consumer(
                [&]()
                {
                    size_t val;
                    size_t last_report = 0;

                    while (total_received.load(std::memory_order_relaxed) < total_items)
                    {
                        if (queue.try_pop(val))
                        {
                            size_t producer_id = val >> 32;
                            assert(producer_id < num_producers);
                            received_counts[producer_id]++;
                            total_received.fetch_add(1, std::memory_order_relaxed);

                            // Progress reporting
                            if (total_received.load() - last_report >= 10000)
                            {
                                last_report = total_received.load();
                                std::cout << "\n  Progress: " << total_received.load() << "/" << total_items << " items";
                            }
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                });

        // Start the race!
        start_flag.store(true, std::memory_order_release);

        // Wait for all threads
        for (auto& t: producers)
        {
            t.join();
        }
        consumer.join();

        // Verify all items received
        for (size_t p = 0; p < num_producers; ++p)
        {
            assert(received_counts[p] == items_per_producer);
        }

        std::cout << "\n  Total push retries due to contention: " << push_failures.load() << "\n";
        std::cout << "PASSED\n";
    }

    // Test 4: Coroutine handles (your actual use case)
    void test_coroutine_handles()
    {
        std::cout << "Test 4: Coroutine handles... ";

        MPSCQueue<std::coroutine_handle<>> queue(16);

        // Create dummy coroutine handles (nullptr for testing)
        std::coroutine_handle<> h1 = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(0x1000));
        std::coroutine_handle<> h2 = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(0x2000));
        std::coroutine_handle<> h3 = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(0x3000));

        assert(queue.try_push(h1));
        assert(queue.try_push(h2));
        assert(queue.try_push(h3));

        std::coroutine_handle<> result;
        assert(queue.try_pop(result) && result.address() == reinterpret_cast<void*>(0x1000));
        assert(queue.try_pop(result) && result.address() == reinterpret_cast<void*>(0x2000));
        assert(queue.try_pop(result) && result.address() == reinterpret_cast<void*>(0x3000));
        assert(!queue.try_pop(result));

        std::cout << "PASSED\n";
    }

    // Test 5: Power-of-2 helper
    void test_power_of_2_helper()
    {
        std::cout << "Test 5: next_power_of_2 helper... ";

        assert(next_power_of_2(0) == 1);
        assert(next_power_of_2(1) == 1);
        assert(next_power_of_2(2) == 2);
        assert(next_power_of_2(3) == 4);
        assert(next_power_of_2(100) == 128);
        assert(next_power_of_2(1000) == 1024);
        assert(next_power_of_2(1024) == 1024);
        assert(next_power_of_2(1025) == 2048);

        std::cout << "PASSED\n";
    }

    void test_non_trivial_objects()
    {
        std::cout << "Test 6: Non-trivial object push/pop... ";

        // Reset counters
        Tracker::alive.store(0);
        Tracker::constructions.store(0);
        Tracker::destructions.store(0);
        Tracker::moves.store(0);
        Tracker::copies.store(0);

        MPSCQueue<Tracker> queue(8);

        // Push several non-trivial objects (move into queue)
        for (int i = 0; i < 4; ++i)
        {
            std::string payload = "obj" + std::to_string(i);
            bool ok = queue.try_push(Tracker(std::move(payload)));
            assert(ok);
        }

        // Pop and validate content
        for (int i = 0; i < 4; ++i)
        {
            Tracker t("dummy");
            bool ok = queue.try_pop(t);
            assert(ok);
            assert(t.data == "obj" + std::to_string(i));
        }

        // Now the queue should be empty
        Tracker tmp("z");
        assert(!queue.try_pop(tmp));

        // Validate no live instances remain (no leaks)
        assert(Tracker::alive.load() == 0);

        std::cout << "PASSED\n";
    }

    void run_all_tests()
    {
        std::cout << "\n=== MPSC Queue Tests ===\n\n";

        test_basic_operations();
        test_fill_and_drain();
        test_coroutine_handles();
        test_power_of_2_helper();
        test_non_trivial_objects();
        test_multi_producer();  // This one is slow, run it last

        std::cout << "\n=== All Tests PASSED ===\n\n";
    }

}  // namespace kio

int main()
{
    kio::run_all_tests();
    return 0;
}
