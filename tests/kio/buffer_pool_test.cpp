#include "kio/buffer_pool.hpp"

#include "kio/context.h"
#include "kio/operations.h"
#include "kio/task.h"

#include <array>
#include <chrono>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/uio.h>

#include <gtest/gtest.h>

using namespace kio;

namespace
{
// Pools here are deliberately tiny (1-16 slots) so every handoff is
// observable, and the one two-region pool never touches its pages: the
// mapping is lazy and the test registers the iovecs without any I/O.

constexpr std::chrono::milliseconds kHoldMs{10};

// fd.hpp intake: UniqueFd wraps the raw fd from open() (explicit ctor)
// and closes it on scope exit.
io::UniqueFd open_temp_file(const char* path)
{
    return io::UniqueFd{::open(path, O_CREAT | O_RDWR | O_TRUNC, 0600)};
}

// ---- task bodies for the scenario tests ----
//
// EXPECT (not ASSERT) inside coroutines: ASSERT_* expands to a plain
// `return`, which is ill-formed in a coroutine body.

// Takes the single slot, sleeps, then releases mid-task -- the release
// is what wakes waiter #1; waiter #1's own lease destruction chains the
// wake to #2, then #3.
Task<> holder_task(io::Context& io)
{
    auto lease = co_await io.acquire_buffer();
    EXPECT_TRUE(lease.has_value());
    co_await async_sleep(io, kHoldMs);
    lease->reset();
    co_return {};
}

Task<> waiter_task(io::Context& io, const int id, std::vector<int>& order)
{
    auto res = co_await io.acquire_buffer();
    EXPECT_TRUE(res.has_value());
    order.push_back(id);
    co_return {};
}

// The releaser itself must not steal the slot it just reserved for the
// woken waiter (surplus rule): try_acquire right after reset() finds
// every free slot spoken for.
Task<> thief_task(io::Context& io, bool& thief_blocked)
{
    auto lease = co_await io.acquire_buffer();
    EXPECT_TRUE(lease.has_value());
    co_await async_sleep(io, kHoldMs);
    lease->reset();
    auto stolen = io.try_acquire_buffer();
    thief_blocked = !static_cast<bool>(stolen);
    co_return {};
}

Task<> parked_waiter(io::Context& io, bool& ok)
{
    auto res = co_await io.acquire_buffer();
    ok = res.has_value();
    co_return {};
}

// Records the cancellation shape close() must produce.
Task<> canceled_waiter(io::Context& io, bool& was_canceled, bool& message_ok)
{
    auto res = co_await io.acquire_buffer();
    was_canceled = !res.has_value();
    if (!res.has_value())
    {
        message_ok = res.error().context() == "pool closed" &&
                     res.error().value() == static_cast<int>(std::errc::operation_canceled);
    }
    co_return {};
}

Task<> holder_with_record(io::Context& io, bool& ok)
{
    auto lease = co_await io.acquire_buffer();
    ok = lease.has_value();
    co_await async_sleep(io, std::chrono::milliseconds(5));
    lease->reset();
    co_return {};
}

Task<> close_after_delay(io::Context& io, RegisteredBufferPool* pool, const std::chrono::milliseconds delay)
{
    co_await async_sleep(io, delay);
    pool->close();
    co_return {};
}

// Lease-first fixed I/O: write a pattern from one slot, read it back
// into a second slot, compare against the caller's pattern.
Task<> fixed_io_task(io::Context& io, io::UniqueFd& fd, const std::array<std::byte, 1000>& pattern, Result<int>& read_n,
                     bool& bytes_match)
{
    auto l1 = co_await io.acquire_buffer();
    EXPECT_TRUE(l1.has_value());
    std::memcpy(l1->data(), pattern.data(), pattern.size());

    const auto w = co_await async_write_fixed(io, fd, *l1, pattern.size(), 0, 0);
    EXPECT_TRUE(w.has_value());

    auto l2 = co_await io.acquire_buffer();
    EXPECT_TRUE(l2.has_value());
    auto r = co_await async_read_fixed(io, fd, *l2, pattern.size(), 0, 0);
    read_n = std::move(r);
    bytes_match = std::memcmp(l2->data(), pattern.data(), pattern.size()) == 0;
    co_return {};
}

Task<> foreign_read_task(io::Context& io, io::UniqueFd& fd, BufferLease& lease)
{
    auto res = co_await async_read_fixed(io, fd, lease, 16, 0, 0);
    (void)res;
    co_return {};
}

// Second acquire parks forever (single-slot pool, nothing ever
// releases): the first lease is the leak run() must detect.
Task<> leak_two(io::Context& io)
{
    auto a = co_await io.acquire_buffer();
    EXPECT_TRUE(a.has_value());
    auto b = co_await io.acquire_buffer();
    (void)b;
    co_return {};
}
}  // namespace

TEST(BufferPoolTest, SlotMathTwoRegions)
{
    // 1 MiB slots x 1537 = ~1.5 GiB of virtual address space. Pages are
    // lazy and nothing is registered here, so no memory is touched beyond
    // the free-list bookkeeping.
    RegisteredBufferPool pool(1 << 20, 1537);

    std::vector<iovec> table;
    const uint16_t base_idx = pool.append_regions(table);

    EXPECT_EQ(table.size(), 2u);
    EXPECT_EQ(base_idx, 0);
    EXPECT_EQ(table[0].iov_len, RegisteredBufferPool::kOneGiB);
    EXPECT_EQ(table[1].iov_len, pool.total_bytes() - RegisteredBufferPool::kOneGiB);

    // Branchless region math: region 0 holds slots [0, 1023], region 1 the
    // remaining 513.
    EXPECT_EQ(pool.registration_index(pool.base() + size_t{0} * pool.slot_size()), 0);
    EXPECT_EQ(pool.registration_index(pool.base() + size_t{1023} * pool.slot_size()), 0);
    EXPECT_EQ(pool.registration_index(pool.base() + size_t{1024} * pool.slot_size()), 1);
    EXPECT_EQ(pool.registration_index(pool.base() + size_t{1536} * pool.slot_size()), 1);
}

TEST(BufferPoolTest, LifoWarmthOrder)
{
    RegisteredBufferPool pool(4096, 4);

    // Seeded slots-1..0, so pops come out 0,1,2,3.
    auto a = pool.try_acquire();
    auto b = pool.try_acquire();
    auto c = pool.try_acquire();
    auto d = pool.try_acquire();
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(c);
    ASSERT_TRUE(d);
    EXPECT_EQ(a.ordinal(), 0);
    EXPECT_EQ(b.ordinal(), 1);
    EXPECT_EQ(c.ordinal(), 2);
    EXPECT_EQ(d.ordinal(), 3);

    // Release 1 then 3: the LIFO top is now 3, then 1 (recently returned
    // slots are the hot ones).
    b.reset();
    d.reset();
    EXPECT_EQ(pool.available(), 2u);
    auto e = pool.try_acquire();
    auto f = pool.try_acquire();
    ASSERT_TRUE(e);
    ASSERT_TRUE(f);
    EXPECT_EQ(e.ordinal(), 3);
    EXPECT_EQ(f.ordinal(), 1);
}

TEST(BufferPoolTest, FifoWakeChainedOrder)
{
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 1});

    std::vector<int> order;
    ctx.spawn(holder_task(ctx));
    ctx.spawn(waiter_task(ctx, 1, order));
    ctx.spawn(waiter_task(ctx, 2, order));
    ctx.spawn(waiter_task(ctx, 3, order));
    ctx.run();

    // Each waiter's lease destruction releases the slot and wakes the next
    // one, so completions come out in spawn order despite the single slot.
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(BufferPoolTest, NoBypassSurplusRule)
{
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 1});

    bool thief_blocked = false;
    bool waiter_ok = false;
    ctx.spawn(thief_task(ctx, thief_blocked));
    ctx.spawn(parked_waiter(ctx, waiter_ok));
    ctx.run();

    EXPECT_TRUE(thief_blocked);
    EXPECT_TRUE(waiter_ok);
}

TEST(BufferPoolTest, CloseCancelsWaiters)
{
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 1});

    // buffer_pool() is a const view and close() is non-const: capture the
    // non-const pool pointer ONCE through a probe lease. probe.pool() must
    // be read before reset() -- reset() nulls the lease's pool pointer.
    auto probe = ctx.try_acquire_buffer();
    ASSERT_TRUE(probe);
    RegisteredBufferPool* pool = probe.pool();
    probe.reset();

    bool holder_ok = false;
    bool w1_canceled = false, w1_msg = false;
    bool w2_canceled = false, w2_msg = false;

    ctx.spawn(holder_with_record(ctx, holder_ok));         // takes the slot, sleeps 5ms
    ctx.spawn(canceled_waiter(ctx, w1_canceled, w1_msg));  // parks
    ctx.spawn(canceled_waiter(ctx, w2_canceled, w2_msg));  // parks
    ctx.spawn(close_after_delay(ctx, pool, std::chrono::milliseconds(2)));
    ctx.run();

    EXPECT_TRUE(holder_ok);
    EXPECT_TRUE(w1_canceled);
    EXPECT_TRUE(w1_msg);
    EXPECT_TRUE(w2_canceled);
    EXPECT_TRUE(w2_msg);
}

TEST(BufferPoolTest, FixedIoRoundtrip)
{
    constexpr const char* kPath = "/tmp/kio_fixed_io_test";
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::UniqueFd fd = open_temp_file(kPath);
    ASSERT_TRUE(fd.valid());

    std::array<std::byte, 1000> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        pattern[i] = static_cast<std::byte>(i * 7 + 3);
    }

    Result<int> read_n{std::unexpect, Error::from_errc(std::errc::operation_canceled)};
    bool bytes_match = false;
    ctx.spawn(fixed_io_task(ctx, fd, pattern, read_n, bytes_match));
    ctx.run();

    ASSERT_TRUE(read_n.has_value());
    EXPECT_EQ(*read_n, 1000);
    EXPECT_TRUE(bytes_match);
    EXPECT_EQ(::unlink(kPath), 0);
}

TEST(BufferPoolTest, ForeignLeaseDies)
{
    io::IoOptions opts;
    opts.entries = 64;
    io::Context a(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::Context b(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::UniqueFd fd = open_temp_file("/tmp/kio_foreign_lease_test");
    ASSERT_TRUE(fd.valid());

    BufferLease lease = a.try_acquire_buffer();
    ASSERT_TRUE(lease);
    ASSERT_EQ(lease.pool(), &*a.buffer_pool());

    // Spawn inside the death-test statement: only the forked child runs it,
    // so the parent's Context b never holds a spawned-but-never-run task
    // (its destructor CHECK-aborts on exactly that).
    EXPECT_DEATH(
        {
            b.spawn(foreign_read_task(b, fd, lease));
            b.run();
        },
        "empty or foreign lease");
    EXPECT_EQ(::unlink("/tmp/kio_foreign_lease_test"), 0);
}

TEST(BufferPoolTest, RunDeadlockAborts)
{
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 1});

    // Same fork-parent reasoning as ForeignLeaseDies: spawn only inside the
    // child.
    EXPECT_DEATH(
        {
            ctx.spawn(leak_two(ctx));
            ctx.run();
        },
        "leases leaked");
}
