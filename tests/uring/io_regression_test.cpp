#include "uring/core/io.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <thread>
#include <vector>

#include <unistd.h>

#include "uring/core/detail/queue.hpp"
#include "uring/tcp_listener.hpp"
#include <gtest/gtest.h>

using namespace kio;
using namespace std::chrono_literals;

namespace
{
Task<int> sleep_then_value(IO& io)
{
    // A negative duration must complete immediately instead of failing with EINVAL.
    if (auto r = co_await io.sleep(-5ms); !r)
        co_return std::unexpected(r.error());
    if (auto r = co_await io.sleep(1ms); !r)
        co_return std::unexpected(r.error());
    co_return 7;
}

Task<int> read_fixed_with(IO& io, Fd& fd, BufferLease& lease)
{
    auto r = co_await io.read_fixed(fd, lease, std::size_t{16});
    if (!r)
        co_return std::unexpected(r.error());
    co_return *r;
}

std::int64_t thread_cpu_ns()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return std::int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
}

Task<void> read_probe(IO& io, Fd& fd, std::span<std::byte> buf, std::atomic<int>& outcome)
{
    auto res = co_await io.read(fd, buf);
    outcome.store(res ? 0 : res.error().value(), std::memory_order_relaxed);
    co_return {};
}
}  // namespace

// sync_wait drives the ring from the calling thread, so it must claim
// SINGLE_ISSUER ownership like run_blocking()/run_once() do. Without that,
// activate() -> arm_wake_read() -> get_sqe() trips the owner-thread assert.
TEST(IoRegressionTest, SyncWaitClaimsRingOwnership)
{
    IO io{0};
    auto r = sync_wait(io, sleep_then_value(io));
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(*r, 7);
}

// One node left after a bounded drain must still be reported as pending, or the
// worker parks with a task stranded in the queue.
TEST(IoRegressionTest, CoroQueueReportsLastNodePendingAfterBoundedDrain)
{
    detail::CoroQueue queue;
    EXPECT_TRUE(queue.empty());

    std::vector<detail::TaskPromiseBase> nodes(129);
    for (auto& n : nodes)
        queue.enqueue(&n);

    EXPECT_EQ(queue.drain([](detail::TaskPromiseBase*) {}, 128), 128u);
    EXPECT_FALSE(queue.empty()) << "one node is still queued";

    EXPECT_EQ(queue.drain([](detail::TaskPromiseBase*) {}, 128), 1u);
    EXPECT_TRUE(queue.empty());
}

// During teardown the worker must park (bounded) instead of spinning while an
// operation is outstanding. A spin burns CPU for the whole grace period.
TEST(IoRegressionTest, DrainParksInsteadOfSpinning)
{
    IoOptions opts;
    opts.shutdown_grace_ms = 600;

    IO io{0, nullptr, opts, std::nullopt};
    std::atomic<int> outcome{-1};

    int fds[2]{};
    ASSERT_EQ(pipe(fds), 0);
    Fd read_fd{fds[0]};
    std::byte buf[16]{};
    io.schedule(read_probe(io, read_fd, std::span(buf), outcome));

    std::atomic<std::int64_t> cpu_ns{-1};
    std::stop_source stop;
    std::jthread worker{[&]
                        {
                            const auto before = thread_cpu_ns();
                            io.run_blocking(stop.get_token());
                            cpu_ns.store(thread_cpu_ns() - before);
                        }};
    std::this_thread::sleep_for(100ms);

    const auto t0 = std::chrono::steady_clock::now();
    stop.request_stop();
    worker.join();
    const auto wall = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(outcome.load(), ECANCELED);
    // The read only completes by cancellation after the full grace period...
    EXPECT_GE(wall, 500ms);
    // ...and the worker must have slept through nearly all of it.
    EXPECT_LT(cpu_ns.load(), std::int64_t{150'000'000}) << "worker busy-waited during drain";

    close(read_fd.Release());
    close(fds[1]);
}

TEST(IoRegressionTest, FixedReadRejectsEmptyAndForeignLeases)
{
    const BufferPoolConfig cfg{.slot_size = 4096, .slots = 4};
    IO io{0, nullptr, {}, cfg};
    IO other{1, nullptr, {}, cfg};

    int fds[2]{};
    ASSERT_EQ(pipe(fds), 0);
    Fd read_fd{fds[0]};
    ASSERT_EQ(write(fds[1], "hello", 5), 5);

    BufferLease empty;
    auto r1 = sync_wait(io, read_fixed_with(io, read_fd, empty));
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().value(), EBADF);

    BufferLease foreign = other.try_acquire_buffer();
    ASSERT_TRUE(foreign);
    auto r2 = sync_wait(io, read_fixed_with(io, read_fd, foreign));
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().value(), EBADF);

    BufferLease own = io.try_acquire_buffer();
    ASSERT_TRUE(own);
    auto r3 = sync_wait(io, read_fixed_with(io, read_fd, own));
    ASSERT_TRUE(r3.has_value()) << r3.error().message();
    EXPECT_EQ(*r3, 5);

    close(fds[1]);
}

TEST(IoRegressionTest, MalformedIpLiteralIsRejectedNotWidenedToAnyAddress)
{
    auto v4 = SocketAddress::V4(8080, "127.0.0.l");
    ASSERT_FALSE(v4.has_value());
    EXPECT_EQ(v4.error().value(), EINVAL);

    auto v6 = SocketAddress::V6(8080, "::zz");
    ASSERT_FALSE(v6.has_value());

    EXPECT_TRUE(SocketAddress::V4(8080, "127.0.0.1").has_value());
    EXPECT_TRUE(SocketAddress::V4(8080).has_value());
    EXPECT_TRUE(SocketAddress::V6(8080, "::1").has_value());

    auto bound = TcpListener::Bind(0, "not-an-ip");
    ASSERT_FALSE(bound.has_value());
    EXPECT_EQ(bound.error().value(), EINVAL);
}

TEST(IoRegressionDeathTest, MovingAnActivatedIoTerminates)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            IO io{0};
            (void)sync_wait(io, sleep_then_value(io)); // activates the ring
            IO moved{std::move(io)};
        },
        "IO move failed");
}
