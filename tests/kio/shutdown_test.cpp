#include "uring/core/io.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>
#include <thread>

#include <unistd.h>

#include "uring/extention/io_pool.hpp"
#include <gtest/gtest.h>

using namespace kio;

namespace
{
// Records how the read ended: success, or the errno the caller observed.
Task<void> read_probe(IO& io, Fd& fd, std::span<std::byte> buf, std::atomic<int>& outcome)
{
    auto res = co_await io.read(fd, buf);
    outcome.store(res ? 0 : res.error().value(), std::memory_order_relaxed);
    co_return {};
}

// Cancelled mid-flight, then tries to issue a *new* operation while unwinding.
// The admission gate must refuse it with ECANCELED rather than let it block.
Task<void> read_then_retry(IO& io, Fd& fd, std::span<std::byte> first, std::span<std::byte> second,
                           std::atomic<int>& first_outcome, std::atomic<int>& retry_outcome)
{
    auto r1 = co_await io.read(fd, first);
    first_outcome.store(r1 ? 0 : r1.error().value(), std::memory_order_relaxed);

    auto r2 = co_await io.read(fd, second);
    retry_outcome.store(r2 ? 0 : r2.error().value(), std::memory_order_relaxed);
    co_return {};
}

// Runs the IO on a worker thread, like IoContext does.
std::jthread make_worker(IO& io, std::stop_source& stop)
{
    return std::jthread([&io, &stop] { io.run_blocking(stop.get_token()); });
}
}  // namespace

// Teardown must not exit the moment the local queues look empty while the
// kernel still owns a blocked operation -- the coroutine behind it would be
// stranded forever. drain_until_quiescent() waits out the grace period, then
// escalates to IORING_ASYNC_CANCEL_ANY and resumes it with -ECANCELED.
TEST(ShutdownTest, StuckOperationIsCancelledAndUnwinds)
{
    IoOptions opts;
    opts.shutdown_grace_ms = 200;

    IO io{0, nullptr, opts, std::nullopt};
    std::atomic<int> outcome{0};

    int fds[2]{};
    ASSERT_EQ(pipe(fds), 0);
    Fd read_fd{fds[0]};
    std::byte buf[32]{};

    // Nothing is ever written to the pipe, so this read blocks forever.
    io.schedule(read_probe(io, read_fd, std::span(buf), outcome));

    std::stop_source stop;
    auto worker = make_worker(io, stop);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const auto t0 = std::chrono::steady_clock::now();
    stop.request_stop();
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(outcome.load(std::memory_order_relaxed), ECANCELED);
    // Bounded by the grace period, not by anything unbounded.
    EXPECT_LT(elapsed, std::chrono::seconds(2));

    close(read_fd.Release());
    close(fds[1]);
}

// The gate exists so a coroutine unwinding from a cancellation cannot submit
// fresh I/O and hang the drain all over again.
TEST(ShutdownTest, NewIoIsRefusedWhileUnwindingFromCancellation)
{
    IoOptions opts;
    opts.shutdown_grace_ms = 200;

    IO io{0, nullptr, opts, std::nullopt};
    std::atomic<int> first_outcome{0};
    std::atomic<int> retry_outcome{-1};

    int fds[2]{};
    ASSERT_EQ(pipe(fds), 0);
    Fd read_fd{fds[0]};
    std::byte b1[32]{};
    std::byte b2[32]{};

    io.schedule(read_then_retry(io, read_fd, std::span(b1), std::span(b2), first_outcome,
                                retry_outcome));

    std::stop_source stop;
    auto worker = make_worker(io, stop);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    stop.request_stop();
    worker.join();

    EXPECT_EQ(first_outcome.load(std::memory_order_relaxed), ECANCELED);
    // The retry must be refused with ECANCELED, not left pending.
    EXPECT_EQ(retry_outcome.load(std::memory_order_relaxed), ECANCELED);

    close(read_fd.Release());
    close(fds[1]);
}

// Regression guard: the always-armed eventfd wake read is internal plumbing, not
// user work. Counting it as outstanding made every shutdown sit out the whole
// grace period (tests went from 11ms to 10s) even with nothing in flight.
TEST(ShutdownTest, IdleShutdownDoesNotWaitOutTheGracePeriod)
{
    IoOptions opts;
    opts.shutdown_grace_ms = 5000;

    IO io{0, nullptr, opts, std::nullopt};

    std::stop_source stop;
    auto worker = make_worker(io, stop);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const auto t0 = std::chrono::steady_clock::now();
    stop.request_stop();
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Nothing outstanding, so quiescence is immediate.
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

// In-flight work that *can* finish should be allowed to finish rather than
// cancelled: the grace period exists so clean shutdowns stay clean.
TEST(ShutdownTest, WorkThatCompletesInTimeIsNotCancelled)
{
    IoOptions opts;
    opts.shutdown_grace_ms = 5000;

    IO io{0, nullptr, opts, std::nullopt};
    std::atomic<int> outcome{0};

    int fds[2]{};
    ASSERT_EQ(pipe(fds), 0);
    Fd read_fd{fds[0]};
    std::byte buf[32]{};

    io.schedule(read_probe(io, read_fd, std::span(buf), outcome));

    std::stop_source stop;
    auto worker = make_worker(io, stop);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Satisfy the read, then immediately ask to stop.
    const std::byte byte{'x'};
    ASSERT_EQ(write(fds[1], &byte, 1), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    stop.request_stop();
    worker.join();

    EXPECT_EQ(outcome.load(std::memory_order_relaxed), 0); // success, not cancelled
    close(read_fd.Release());
    close(fds[1]);
}