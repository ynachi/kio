#include "uring/core/io.h"

#include <atomic>
#include <future>
#include <latch>
#include <limits>
#include <type_traits>

#include <sys/mman.h>

#include "uring/extention/io_pool.hpp"
#include "uring/tcp_listener.hpp"
#include <gtest/gtest.h>

using namespace URing;
using namespace std::chrono_literals;

namespace
{
struct CountDestruction
{
    std::atomic<int>* counter;
    explicit CountDestruction(std::atomic<int>& count) : counter(&count) {}
    CountDestruction(CountDestruction&& other) noexcept : counter(std::exchange(other.counter, nullptr)) {}
    ~CountDestruction()
    {
        if (counter)
            ++*counter;
    }
};

Task<void> increment(std::atomic<int>& count)
{
    ++count;
    co_return {};
}
Task<void> unused_root(UniqueFd fd, CountDestruction guard)
{
    (void)fd;
    (void)guard;
    co_return {};
}
Task<void> close_on_stop(IO& io, UniqueFd fd, std::stop_source& stop, std::atomic<int>& error)
{
    stop.request_stop();
    auto result = co_await io.close(std::move(fd));
    error = result ? 0 : result.error().Value();
    co_return {};
}
Task<int> nested_read(IO& io, UniqueFd& fd)
{
    std::byte buffer[16];
    co_return co_await io.read(fd, buffer);
}
Task<void> blocked_read(IO& io, UniqueFd fd, std::latch& entered, std::atomic<int>& error, CountDestruction guard)
{
    (void)guard;
    entered.count_down();
    auto result = co_await nested_read(io, fd);
    error = result ? 0 : result.error().Value();
    co_return {};
}
Task<void> blocked_sleep(IO& io, std::latch& entered, std::atomic<int>& error)
{
    entered.count_down();
    auto result = co_await io.sleep(1h);
    error = result ? 0 : result.error().Value();
    // Cleanup code can attempt I/O and must receive cancellation immediately.
    auto retry = co_await io.sleep(1h);
    EXPECT_FALSE(retry);
    EXPECT_EQ(retry.error().Value(), ECANCELED);
    co_return {};
}
Task<void> blocked_accept(IO& io, UniqueFd listener, std::latch& entered, std::atomic<int>& error)
{
    entered.count_down();
    auto result = co_await io.accept(listener);
    error = result ? 0 : result.error().Value();
    co_return {};
}
Task<void> wrong_worker(IO& other, UniqueFd fd, std::atomic<int>& error)
{
    auto result = co_await other.sleep(1ms);
    error = result ? 0 : result.error().Value();
    (void)fd;
    co_return {};
}
Task<void> buffer_round_trip(IO& io)
{
    URING_TRY(auto buffer, io.take_fixed_buffer(4096));
    UniqueFd fd{memfd_create("fixed-buffer-test", MFD_CLOEXEC)};
    EXPECT_TRUE(fd.Valid());
    std::fill(buffer.data().begin(), buffer.data().end(), std::byte{0x5a});
    URING_TRY(auto written, co_await io.write_fixed(fd, buffer, buffer.size(), 0));
    EXPECT_EQ(written, 4096);
    std::fill(buffer.data().begin(), buffer.data().end(), std::byte{});
    URING_TRY(auto read, co_await io.read_fixed(fd, buffer, buffer.size(), 0));
    EXPECT_EQ(read, 4096);
    EXPECT_TRUE(std::ranges::all_of(buffer.data(), [](std::byte byte) { return byte == std::byte{0x5a}; }));
    auto exhausted = io.take_fixed_buffer(4096);
    EXPECT_FALSE(exhausted);
    co_return {};
}
Task<UniqueFd> return_fd(IO& io)
{
    co_return co_await io.open("/dev/null", O_RDONLY);
}
Task<int> immediate_value()
{
    co_return 42;
}
Task<void> nested_driver(IO& io)
{
    EXPECT_THROW((void)sync_wait(io, immediate_value()), std::logic_error);
    EXPECT_THROW(io.run({}), std::logic_error);
    co_return {};
}
Task<FixedBuffer> borrow_buffer(IO& io)
{
    co_return io.take_fixed_buffer(4096);
}
Task<void> use_foreign_buffer(IO& io, FixedBuffer& buffer)
{
    UniqueFd fd{memfd_create("foreign-buffer", MFD_CLOEXEC)};
    auto result = co_await io.write_fixed(fd, buffer, 0);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().Value(), EINVAL);
    co_return {};
}
Task<void> successful_read(IO& io, UniqueFd fd, std::stop_source& stop, std::atomic<int>& value)
{
    std::byte byte{};
    auto result = co_await io.read(fd, std::span{&byte, 1});
    value = result ? static_cast<int>(byte) : -result.error().Value();
    stop.request_stop();
    co_return {};
}
}  // namespace

TEST(IoLifecycleTest, ReactorAddressIsStable)
{
    static_assert(!std::is_move_constructible_v<IO>);
    static_assert(!std::is_copy_constructible_v<IO>);
    static_assert(!std::is_constructible_v<FixedBuffer, uint32_t, uint32_t, std::span<std::byte>, FixedBufferPool*>);
}

TEST(IoLifecycleTest, BufferConfigurationRejectsOverflow)
{
    EXPECT_THROW((FixedBufferPool{{{4096, 65537}}}), std::invalid_argument);
    EXPECT_THROW((FixedBufferPool{{{std::numeric_limits<size_t>::max() - 4095, 2}}}), std::invalid_argument);
    EXPECT_THROW((FixedBufferPool{{{4096, 0}}}), std::invalid_argument);
}

TEST(IoLifecycleTest, ReturningBufferClearsItsView)
{
    FixedBufferPool pool{{{4096, 1}}};
    auto original = pool.take(1);
    ASSERT_TRUE(original);
    auto buffer = std::move(*original);
    EXPECT_TRUE(original->data().empty());
    buffer.release();
    EXPECT_FALSE(buffer.valid());
    EXPECT_TRUE(buffer.data().empty());
    EXPECT_TRUE(pool.take(1));
}

TEST(IoLifecycleTest, SyncWaitRejectsNestedDrivers)
{
    IO io(0);
    EXPECT_TRUE(sync_wait(io, nested_driver(io)));
    auto result = sync_wait(io, immediate_value());
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 42);
}

TEST(IoLifecycleTest, DestructionReleasesNeverStartedRoots)
{
    std::atomic<int> destroyed{0};
    const int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);
    {
        IO io(0);
        EXPECT_TRUE(io.schedule(unused_root(UniqueFd{raw}, CountDestruction{destroyed})));
    }
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(fcntl(raw, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(IoLifecycleTest, ShutdownDrainsSubmittedClose)
{
    IO io(0);
    const int raw = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw, 0);
    std::stop_source stop;
    std::atomic<int> error{-1};
    ASSERT_TRUE(io.schedule(close_on_stop(io, UniqueFd{raw}, stop, error)));
    io.run(stop.get_token());
    EXPECT_EQ(error, 0);
    EXPECT_EQ(fcntl(raw, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(IoLifecycleTest, StopUnwindsNestedReadAndReleasesDescriptor)
{
    IO io(0);
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
    UniqueFd peer{sockets[1]};
    std::atomic<int> destroyed{0}, error{-1};
    std::latch entered{1};
    ASSERT_TRUE(io.schedule(blocked_read(io, UniqueFd{sockets[0]}, entered, error, CountDestruction{destroyed})));
    std::jthread runner([&](std::stop_token stop) { io.run(stop); });
    entered.wait();
    runner.request_stop();
    runner.join();
    EXPECT_EQ(error, ECANCELED);
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(fcntl(sockets[0], F_GETFD), -1);
}

TEST(IoLifecycleTest, StopCancelsSleepAndRejectsFurtherIo)
{
    IO io(0);
    std::latch entered{1};
    std::atomic<int> error{-1};
    ASSERT_TRUE(io.schedule(blocked_sleep(io, entered, error)));
    std::jthread runner([&](std::stop_token stop) { io.run(stop); });
    entered.wait();
    runner.request_stop();
    runner.join();
    EXPECT_EQ(error, ECANCELED);
    std::atomic<int> ran{0};
    EXPECT_FALSE(io.schedule(increment(ran)));
    EXPECT_EQ(ran, 0);
}

TEST(IoLifecycleTest, StopCancelsAccept)
{
    IO io(0);
    auto listener = TcpListener::Bind(0, "127.0.0.1");
    ASSERT_TRUE(listener);
    std::latch entered{1};
    std::atomic<int> error{-1};
    ASSERT_TRUE(io.schedule(blocked_accept(io, std::move(*listener), entered, error)));
    std::jthread runner([&](std::stop_token stop) { io.run(stop); });
    entered.wait();
    runner.request_stop();
    runner.join();
    EXPECT_EQ(error, ECANCELED);
}

TEST(IoLifecycleTest, StopBeforeRunDrainsAcceptedRoots)
{
    IO io(0, nullptr, {.entries = 8, .batch_max_size = 1});
    std::atomic<int> ran{0};
    for (int i = 0; i < 64; ++i)
        ASSERT_TRUE(io.schedule(increment(ran)));
    std::stop_source stop;
    stop.request_stop();
    io.run(stop.get_token());
    EXPECT_EQ(ran, 64);
    EXPECT_THROW(io.run(stop.get_token()), std::logic_error);
}

TEST(IoLifecycleTest, IdleStopNeverDependsOnAnotherSubmission)
{
    for (int iteration = 0; iteration < 32; ++iteration)
    {
        IO io(0, nullptr, {.entries = 8});
        std::jthread runner([&](std::stop_token stop) { io.run(stop); });
        runner.request_stop();
        runner.join();
    }
}

TEST(IoLifecycleTest, IoCannotMigrateAnAwaitedChain)
{
    IO io(0), other(1);
    std::atomic<int> error{-1};
    ASSERT_TRUE(sync_wait(io, wrong_worker(other, UniqueFd{}, error)));
    EXPECT_EQ(error, EXDEV);
}

TEST(IoLifecycleTest, SyncWaitSupportsRepeatedCallsAndMoveOnlyResults)
{
    IO io(0);
    auto value = sync_wait(io, immediate_value());
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 42);
    auto fd = sync_wait(io, return_fd(io));
    ASSERT_TRUE(fd);
    EXPECT_TRUE(fd->Valid());
}

TEST(IoLifecycleTest, RegisteredBuffersRoundTripAndAreReturned)
{
    IO io(0, nullptr,
          {
    },
          {{4096, 1}});
    EXPECT_TRUE(sync_wait(io, buffer_round_trip(io)));
    EXPECT_TRUE(sync_wait(io, buffer_round_trip(io)));
}

TEST(IoLifecycleTest, RegisteredBuffersBelongToTheirReactor)
{
    IO first(0, nullptr,
             {
    },
             {{4096, 1}}),
        second(1, nullptr, {}, {{4096, 1}});
    auto buffer = sync_wait(first, borrow_buffer(first));
    ASSERT_TRUE(buffer);
    EXPECT_TRUE(sync_wait(second, use_foreign_buffer(second, *buffer)));
}

TEST(IoLifecycleTest, SuccessfulCompletionIsPreservedDuringStop)
{
    IO io(0);
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    UniqueFd peer{sockets[1]};
    ASSERT_EQ(::write(peer.Get(), "Z", 1), 1);
    std::stop_source stop;
    std::atomic<int> value{0};
    ASSERT_TRUE(io.schedule(successful_read(io, UniqueFd{sockets[0]}, stop, value)));
    io.run(stop.get_token());
    EXPECT_EQ(value, 'Z');
}

TEST(IoLifecycleTest, ConcurrentSubmissionAndShutdownReleaseEveryTask)
{
    IO io(0, nullptr, {.entries = 8, .batch_max_size = 1});
    std::atomic<int> destroyed{0};
    std::jthread runner([&](std::stop_token stop) { io.run(stop); });
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < 4; ++producer)
        producers.emplace_back(
            [&]
            {
                for (int i = 0; i < 128; ++i)
                    io.schedule(unused_root(UniqueFd{}, CountDestruction{destroyed}));
            });
    runner.request_stop();
    for (auto& producer : producers)
        producer.join();
    runner.join();
    EXPECT_EQ(destroyed, 512);
}

TEST(IoLifecycleTest, SmallRingDrainsManyPendingReads)
{
    IO io(0, nullptr, {.entries = 8, .batch_max_size = 1});
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
    UniqueFd reader{sockets[0]}, peer{sockets[1]};
    constexpr int count = 48;
    std::latch entered{count};
    std::atomic<int> destroyed{0};
    std::array<std::atomic<int>, count> errors;
    for (auto& error : errors)
        error = -1;
    for (auto& error : errors)
    {
        const int duplicate = dup(reader.Get());
        ASSERT_GE(duplicate, 0);
        ASSERT_TRUE(io.schedule(blocked_read(io, UniqueFd{duplicate}, entered, error, CountDestruction{destroyed})));
    }
    std::jthread runner([&](std::stop_token stop) { io.run(stop); });
    entered.wait();
    runner.request_stop();
    runner.join();
    EXPECT_EQ(destroyed, count);
    for (const auto& error : errors)
        EXPECT_EQ(error, ECANCELED);
}
