#include "uring/core/fiber_sync.hpp"
#include "uring/core/fiber_io.hpp"
#include "uring/core/io.h"
#include "uring/extention/io_pool.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>

#include <gtest/gtest.h>

using namespace URing;

namespace
{

IoOptions fast_opts()
{
    IoOptions o;
    o.tick_timeout_ms = 1;
    return o;
}

std::filesystem::path temp_path()
{
    const auto name = std::string{"kio_sync_test_"} + std::to_string(::getpid()) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

// ─── FiberMutex ───────────────────────────────────────────────────────────────

// Fiber A holds the mutex across two I/O suspensions.
// Fiber B tries to acquire while A holds it.
// Verify: B only gets the lock after A's explicit unlock().
TEST(FiberMutexTest, EnforcesMutualExclusion)
{
    IO io(0, nullptr, fast_opts(), {{4096, 2}});

    FiberMutex             mu;
    std::vector<std::string> log;    // single-threaded access — no sync needed
    std::atomic<int>         done{0};
    const auto               path = temp_path();

    // Fiber A: lock → open file (suspends) → close (suspends) → unlock
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            mu.lock(fio);
            log.push_back("A:locked");
            FIBER_TRY(auto fd, fio.open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644));
            log.push_back("A:io");
            FIBER_TRY_VOID(fio.close(std::move(fd)));
            log.push_back("A:unlocking");
            mu.unlock(fio);
            done.fetch_add(1, std::memory_order_release);
            return {};
        });

    // Fiber B: must wait for A's unlock before acquiring
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            mu.lock(fio);
            log.push_back("B:locked");
            mu.unlock(fio);
            done.fetch_add(1, std::memory_order_release);
            return {};
        });

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (done.load(std::memory_order_acquire) < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    runner.request_stop();
    runner.join();
    std::filesystem::remove(path);

    ASSERT_EQ(done.load(), 2) << "not all fibers completed";
    ASSERT_EQ(log.size(), 4u);
    EXPECT_EQ(log[0], "A:locked");
    EXPECT_EQ(log[1], "A:io");       // B did NOT sneak in between
    EXPECT_EQ(log[2], "A:unlocking");
    EXPECT_EQ(log[3], "B:locked");   // B only got in after A unlocked
}

// try_lock succeeds when free and fails when held.
TEST(FiberMutexTest, TryLock)
{
    IO io(0, nullptr, fast_opts(), {});

    FiberMutex        mu;
    std::atomic<bool> done{false};

    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            EXPECT_TRUE(mu.try_lock());   // free  → succeeds
            EXPECT_FALSE(mu.try_lock());  // held  → fails
            mu.unlock(fio);
            EXPECT_TRUE(mu.try_lock());   // free again → succeeds
            mu.unlock(fio);
            done.store(true, std::memory_order_release);
            return {};
        });

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    runner.request_stop();
    runner.join();
    ASSERT_TRUE(done.load());
}

// FiberLockGuard releases the mutex on scope exit even through FIBER_TRY_VOID.
TEST(FiberMutexTest, LockGuardReleasesOnExit)
{
    IO io(0, nullptr, fast_opts(), {{4096, 2}});

    FiberMutex        mu;
    std::atomic<bool> done{false};
    std::atomic<bool> second_acquired{false};
    const auto        path = temp_path();

    // Fiber A: acquire via guard, do I/O, let guard release on scope exit
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            {
                FiberLockGuard g{mu, fio};
                FIBER_TRY(auto fd, fio.open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644));
                FIBER_TRY_VOID(fio.close(std::move(fd)));
            }  // guard releases here
            done.store(true, std::memory_order_release);
            return {};
        });

    // Fiber B: verify it can acquire after A's guard releases
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            FiberLockGuard g{mu, fio};  // suspends until A's guard releases
            second_acquired.store(true, std::memory_order_release);
            return {};
        });

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!done.load(std::memory_order_acquire) || !second_acquired.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    runner.request_stop();
    runner.join();
    std::filesystem::remove(path);

    ASSERT_TRUE(done.load());
    ASSERT_TRUE(second_acquired.load());
}

// ─── FiberSemaphore ───────────────────────────────────────────────────────────

// Semaphore starts at 0.  Fiber B calls wait() and parks.
// Fiber A completes I/O then calls post() — B must only run after that.
TEST(FiberSemaphoreTest, SignalingOrdering)
{
    IO io(0, nullptr, fast_opts(), {{4096, 2}});

    FiberSemaphore           sem{0};
    std::vector<std::string> log;
    std::atomic<int>         done{0};
    const auto               path = temp_path();

    // Fiber A: do I/O then post
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            FIBER_TRY(auto fd, fio.open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644));
            FIBER_TRY_VOID(fio.close(std::move(fd)));
            log.push_back("A:posted");
            sem.post(fio);
            done.fetch_add(1, std::memory_order_release);
            return {};
        });

    // Fiber B: wait on the semaphore
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            sem.wait(fio);
            log.push_back("B:resumed");
            done.fetch_add(1, std::memory_order_release);
            return {};
        });

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (done.load(std::memory_order_acquire) < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    runner.request_stop();
    runner.join();
    std::filesystem::remove(path);

    ASSERT_EQ(done.load(), 2);
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], "A:posted");
    EXPECT_EQ(log[1], "B:resumed");
}

// Semaphore starts at 2.  Two fibers wait() without blocking;
// the third parks and is released by a post().
TEST(FiberSemaphoreTest, CountingBehavior)
{
    IO io(0, nullptr, fast_opts(), {});

    FiberSemaphore   sem{2};
    std::atomic<int> passed{0};
    std::atomic<int> done{0};

    for (int i = 0; i < 3; ++i)
    {
        io.spawn_fiber(
            [&](FiberIO& fio) -> Result<void>
            {
                sem.wait(fio);
                passed.fetch_add(1, std::memory_order_release);
                done.fetch_add(1, std::memory_order_release);
                return {};
            });
    }

    // Fourth fiber posts after the first two have run
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            // Let the first two grab the slots; the third is waiting
            sem.post(fio);
            done.fetch_add(1, std::memory_order_release);
            return {};
        });

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (done.load(std::memory_order_acquire) < 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    runner.request_stop();
    runner.join();

    ASSERT_EQ(done.load(), 4);
    EXPECT_EQ(passed.load(), 3);   // all three waiters passed
    EXPECT_EQ(sem.value(), 0);     // all slots consumed
}

// ─── FiberChannel ─────────────────────────────────────────────────────────────

// Producer sends 5 integers through a capacity-2 channel.
// Consumer receives all 5.  Sends 3-5 block until the consumer drains space.
TEST(FiberChannelTest, BoundedSendRecvOrdering)
{
    IO io(0, nullptr, fast_opts(), {});

    FiberChannel<int, 2> ch;
    std::vector<int>     received;
    std::atomic<bool>    done{false};

    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            for (int i = 0; i < 5; ++i)
                ch.send(fio, i);
            return {};
        });

    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            for (int i = 0; i < 5; ++i)
                received.push_back(ch.recv(fio));
            done.store(true, std::memory_order_release);
            return {};
        });

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    runner.request_stop();
    runner.join();

    ASSERT_TRUE(done.load()) << "consumer did not complete";
    ASSERT_EQ(received.size(), 5u);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(received[i], i) << "wrong value at index " << i;
}

// Consumer calls recv() before any data is available.
// Verify it blocks and gets the value after the producer sends it.
TEST(FiberChannelTest, RecvBlocksUntilData)
{
    IO io(0, nullptr, fast_opts(), {});

    FiberChannel<std::string, 4> ch;
    std::string              got;
    std::atomic<bool>        consumer_done{false};
    std::atomic<bool>        producer_done{false};

    // Consumer first — will park immediately
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            got = ch.recv(fio);
            consumer_done.store(true, std::memory_order_release);
            return {};
        });

    // Producer second — sends after a tick
    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            ch.send(fio, std::string{"hello"});
            producer_done.store(true, std::memory_order_release);
            return {};
        });

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!consumer_done.load(std::memory_order_acquire) ||
            !producer_done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    runner.request_stop();
    runner.join();

    ASSERT_TRUE(consumer_done.load());
    ASSERT_TRUE(producer_done.load());
    EXPECT_EQ(got, "hello");
}
