#include "uring/core/io.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

#include <unistd.h>

#include "uring/extention/io_pool.hpp"
#include <gtest/gtest.h>

using namespace kio;

namespace
{
template <std::size_t N>
Task<void> record_worker(IO& target, std::array<std::atomic<int>, N>& observed, std::atomic<int>& ran)
{
    observed[target.id()].fetch_add(1, std::memory_order_relaxed);
    ran.fetch_add(1, std::memory_order_acq_rel);
    co_return {};
}

template <std::size_t N>
Task<void> schedule_record_on(IO& target, std::array<std::atomic<int>, N>& observed, std::atomic<int>& ran)
{
    target.schedule(record_worker(target, observed, ran));
    co_return {};
}
}  // namespace

TEST(IoContextRemoteTest, ScheduleRunsOnTargetWorker)
{
    IoOptions opts;

    IoContext ctx(2, opts);
    std::atomic<int> ran{0};
    std::atomic<int> observed_worker{-1};

    // Define a task that records the ID of the worker it runs on
    auto task = [&](IO& target) -> Task<void>
    {
        observed_worker.store(static_cast<int>(target.id()), std::memory_order_relaxed);
        ran.fetch_add(1, std::memory_order_relaxed);
        co_return {};
    };

    // Schedule on worker 1 from the main thread
    ctx.worker(1).schedule(task(ctx.worker(1)));

    // Busy wait for completion with timeout
    auto start = std::chrono::steady_clock::now();
    while (ran.load() == 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(2))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ctx.join();

    EXPECT_EQ(ran.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(observed_worker.load(std::memory_order_relaxed), 1);
}

TEST(IoContextRemoteTest, WorkersCanScheduleOnAnotherWorker)
{
    IoOptions opts;

    constexpr std::size_t kWorkers = 4;
    IoContext ctx(kWorkers, opts);
    std::array<std::atomic<int>, kWorkers> observed{};
    std::atomic<int> ran{0};

    for (auto& value : observed)
    {
        value.store(0, std::memory_order_relaxed);
    }

    // Chain scheduling: 0 -> 1, 1 -> 2, 2 -> 3, 3 -> 0
    for (std::size_t i = 0; i < kWorkers; ++i)
    {
        std::size_t next = (i + 1) % kWorkers;
        ctx.worker(i).schedule(schedule_record_on(ctx.worker(next), observed, ran));
    }

    // Wait for all tasks to complete
    auto start = std::chrono::steady_clock::now();
    while (ran.load() < static_cast<int>(kWorkers) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ctx.join();

    EXPECT_EQ(ran.load(std::memory_order_relaxed), static_cast<int>(kWorkers));
    for (const auto& value : observed)
    {
        EXPECT_EQ(value.load(std::memory_order_relaxed), 1);
    }
}

namespace
{
Task<void> read_one(IO& io, Fd& fd, std::span<std::byte> buf, std::atomic<int>& completed)
{
    auto res = co_await io.read(fd, buf);
    if (res)
    {
        completed.fetch_add(1, std::memory_order_relaxed);
    }
    co_return {};
}
}  // namespace

// tick() resumes at most kMaxResumesPerTick (128) completions per tick and
// carries the rest into the next tick. Only real CQ completions can overflow
// that cap -- the MPSC drain is already bounded by batch_max_size -- so this
// uses a burst of reads that all complete at once. Every one of them must
// still run: a dropped remainder would strand those coroutines at final_suspend
// forever, and a resumed-twice handle is a use-after-free.
TEST(IOResumeCapTest, CompletionBurstLargerThanResumeCapRunsFully)
{
    IoOptions opts;
    // Let one drain hand over more than the resume cap.
    opts.batch_max_size = 4096;

    IO io{0, nullptr, opts, std::nullopt};
    std::atomic<int> completed{0};

    constexpr std::size_t kReads = 1000;
    constexpr std::size_t kChunk = 32;
    int fds[2]{};
    ASSERT_EQ(pipe(fds), 0);

    // Pre-fill the pipe so every read completes immediately. This must stay
    // under the 64 KiB pipe capacity: nothing drains until run_blocking() runs,
    // so a larger write would block here before any read is ever submitted.
    static_assert(kReads * kChunk < 65536, "payload must fit in the pipe buffer");
    std::vector<std::byte> payload(kReads * kChunk);
    ASSERT_EQ(write(fds[1], payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));

    Fd read_fd{fds[0]}; // owns fds[0]; released back at the end
    std::vector<std::byte> buffers(kReads * kChunk);

    // Enqueue everything before the loop starts: one producer, no concurrent
    // drain. (Producing from another thread while the worker drains is a
    // separate, pre-existing CoroQueue race -- see IOResumeTest below.)
    for (std::size_t i = 0; i < kReads; ++i)
    {
        io.schedule(read_one(io,
                             read_fd,
                             std::span(buffers).subspan(i * kChunk, kChunk),
                             completed));
    }

    std::stop_source stop;
    std::jthread worker{[&io, &stop] { io.run_blocking(stop.get_token()); }};

    auto start = std::chrono::steady_clock::now();
    while (completed.load(std::memory_order_relaxed) < static_cast<int>(kReads) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop.request_stop();
    worker.join();

    EXPECT_EQ(completed.load(std::memory_order_relaxed), static_cast<int>(kReads));
    close(read_fd.Release());
    close(fds[1]);
}

// Same burst, but produced from another thread while the worker drains the MPSC
// queue concurrently -- the cross-thread scheduling path under load.
TEST(IOResumeCapTest, ConcurrentProducerBurstRunsFully)
{
    IoOptions opts;
    opts.batch_max_size = 4096;

    IoContext ctx(1, opts);
    std::atomic<int> completed{0};

    constexpr std::size_t kReads = 1000;
    constexpr std::size_t kChunk = 32;
    int fds[2]{};
    ASSERT_EQ(pipe(fds), 0);

    static_assert(kReads * kChunk < 65536, "payload must fit in the pipe buffer");
    std::vector<std::byte> payload(kReads * kChunk);
    ASSERT_EQ(write(fds[1], payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));

    Fd read_fd{fds[0]};
    std::vector<std::byte> buffers(kReads * kChunk);
    for (std::size_t i = 0; i < kReads; ++i)
    {
        ctx.worker(0).schedule(
            read_one(ctx.worker(0),
                     read_fd,
                     std::span(buffers).subspan(i * kChunk, kChunk),
                     completed));
    }

    auto start = std::chrono::steady_clock::now();
    while (completed.load(std::memory_order_relaxed) < static_cast<int>(kReads) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ctx.join();

    EXPECT_EQ(completed.load(std::memory_order_relaxed), static_cast<int>(kReads));
    close(read_fd.Release());
    close(fds[1]);
}
