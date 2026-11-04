#include "kio/buffer_pool.hpp"

#include "kio/context.h"
#include "kio/operations.h"
#include "kio/task.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <set>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/uio.h>

#include "test_util.hpp"
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
io::UniqueFd open_temp_file(const std::filesystem::path& path)
{
    return io::UniqueFd{::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600)};
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

Task<> foreign_read_task(io::Context& io, const io::UniqueFd& fd, BufferLease& lease)
{
    const auto res = co_await async_read_fixed(io, fd, lease, 16, 0, 0);
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
    EXPECT_EQ(order, (std::vector{1, 2, 3}));
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
    test::TempDir dir;
    const auto file_path = dir.path() / "fixed_io_test.bin";
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::UniqueFd fd = open_temp_file(file_path);
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
}

TEST(BufferPoolTest, ForeignLeaseDies)
{
    test::TempDir dir;
    const auto file_path = dir.path() / "foreign_lease_test.bin";
    io::IoOptions opts;
    opts.entries = 64;
    io::Context a(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::Context b(0, nullptr, opts, kio::BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::UniqueFd fd = open_temp_file(file_path);
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

// ===========================================================================
// Construction validation
// ===========================================================================

TEST(BufferPoolTest, ConstructorRejectsBadSlotGeometry)
{
    // non-power-of-two slot size
    EXPECT_THROW(RegisteredBufferPool(3000, 4), std::invalid_argument);
    // below one page
    EXPECT_THROW(RegisteredBufferPool(2048, 4), std::invalid_argument);
    // 2 GiB slots: cannot slice into 1 GiB registration chunks
    EXPECT_THROW(RegisteredBufferPool(size_t{1} << 31u, 4), std::invalid_argument);
    // zero slots
    EXPECT_THROW(RegisteredBufferPool(4096, 0), std::invalid_argument);
}

// 1 GiB x 2^18 slots asks for 256 TiB of contiguous address space: mmap
// must refuse on 47-bit VA machines. Skipped (not failed) where the
// platform would grant the mapping.
TEST(BufferPoolTest, ExhaustedAddressSpaceThrowsSystemError)
{
    try
    {
        RegisteredBufferPool pool(size_t{1} << 30, 1U << 18);
    }
    catch (const std::system_error& e)
    {
        EXPECT_EQ(e.code().value(), ENOMEM);
        SUCCEED() << "mmap refused with ENOMEM as expected";
        return;
    }
    GTEST_SKIP() << "platform granted a 256 TiB mapping (57-bit VA / aggressive overcommit)";
}

// ===========================================================================
// Geometry
// ===========================================================================

TEST(BufferPoolTest, GeometryIsTwoMiBAligned)
{
    RegisteredBufferPool pool(4096, 8);

    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pool.base()) % RegisteredBufferPool::kHugePageSize, 0u);
    EXPECT_EQ(pool.allocated_bytes() % RegisteredBufferPool::kHugePageSize, 0u);
    EXPECT_GE(pool.allocated_bytes(), pool.total_bytes());
    EXPECT_EQ(pool.total_bytes(), 4096u * 8u);
}

TEST(BufferPoolTest, OrdinalsAreUniqueAndDense)
{
    RegisteredBufferPool pool(4096, 8);

    std::array<BufferLease, 8> leases{
        pool.try_acquire(), pool.try_acquire(), pool.try_acquire(), pool.try_acquire(),
        pool.try_acquire(), pool.try_acquire(), pool.try_acquire(), pool.try_acquire(),
    };
    std::set<uint32_t> ordinals;
    for (const auto& l : leases)
    {
        ASSERT_TRUE(l);
        ordinals.insert(l.ordinal());
        EXPECT_EQ(l.data(), pool.base() + size_t{l.ordinal()} * pool.slot_size());
        EXPECT_EQ(l.size(), 4096u);
    }
    EXPECT_EQ(ordinals.size(), 8u);

    // Exhausted: the next non-blocking acquire is an empty lease.
    EXPECT_FALSE(pool.try_acquire());
    EXPECT_EQ(pool.available(), 0u);
}

TEST(BufferPoolTest, LeaseMoveSemantics)
{
    RegisteredBufferPool pool(4096, 2);

    auto a = pool.try_acquire();
    auto b = pool.try_acquire();
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(pool.available(), 0u);

    // Move-construct: the source empties, nothing is released.
    auto moved{std::move(a)};
    EXPECT_TRUE(moved);
    EXPECT_FALSE(a);
    EXPECT_EQ(pool.available(), 0u);

    // Move-assign: the target's OLD slot is released first.
    moved = std::move(b);
    EXPECT_NE(moved.ordinal(), 0u);   // it took b's slot, not the one it held
    EXPECT_EQ(pool.available(), 1u);  // the vacated slot went back

    // reset() is idempotent.
    moved.reset();
    EXPECT_EQ(pool.available(), 2u);
    moved.reset();
    EXPECT_EQ(pool.available(), 2u);
}

TEST(BufferPoolDeathTest, LeaseBoundsChecksTerminate)
{
    RegisteredBufferPool pool(4096, 1);
    auto lease = pool.try_acquire();
    ASSERT_TRUE(lease);

    EXPECT_DEATH((void)lease.first(4097), "exceeds slot size");
    EXPECT_DEATH((void)lease.subspan(4000, 100), "outside slot");
}

// ===========================================================================
// Three-region registration math (2.5 GiB of lazy virtual space)
// ===========================================================================

TEST(BufferPoolTest, ThreeRegionAppendAndBoundaryIndexes)
{
    // 64 KiB x 40960 slots = 2.5 GiB: three registration chunks of
    // 1 GiB + 1 GiB + 0.5 GiB. Pages are never touched (lazy mapping).
    RegisteredBufferPool pool(65536, 40960);

    std::vector<iovec> table{
        {nullptr, 1},
        {nullptr, 1}
    };  // two foreign entries
    const uint16_t first = pool.append_regions(table);

    EXPECT_EQ(first, 2u);
    ASSERT_EQ(table.size(), 5u);
    EXPECT_EQ(table[2].iov_len, RegisteredBufferPool::kOneGiB);
    EXPECT_EQ(table[3].iov_len, RegisteredBufferPool::kOneGiB);
    EXPECT_EQ(table[4].iov_len, pool.total_bytes() - 2 * RegisteredBufferPool::kOneGiB);

    // Chunk-boundary bytes resolve to the right table index.
    EXPECT_EQ(pool.registration_index(pool.base()), 2u);
    EXPECT_EQ(pool.registration_index(pool.base() + RegisteredBufferPool::kOneGiB - 1), 2u);
    EXPECT_EQ(pool.registration_index(pool.base() + RegisteredBufferPool::kOneGiB), 3u);
    EXPECT_EQ(pool.registration_index(pool.base() + 2 * RegisteredBufferPool::kOneGiB), 4u);
    EXPECT_EQ(pool.registration_index(pool.base() + pool.total_bytes() - 1), 4u);

    // No slot may straddle a 1 GiB registration boundary (Invariant 2).
    // The slot size (64 KiB = 2^16) evenly divides 1 GiB (2^30), giving exactly 16384 slots per GiB.
    static_assert((RegisteredBufferPool::kOneGiB % 65536) == 0);
    for (const uint32_t ord : {0u, 16383u, 16384u, 32767u, 32768u, 40959u})
    {
        const size_t off = size_t{ord} * 65536;
        EXPECT_EQ(off >> 30, (off + 65535) >> 30) << "slot " << ord << " straddles a chunk boundary";
    }
}

TEST(BufferPoolDeathTest, RegistrationIndexOutsideMappingTerminates)
{
    RegisteredBufferPool pool(4096, 1);
    EXPECT_DEATH((void)pool.registration_index(pool.base() + pool.total_bytes()), "outside the pool mapping");
}

TEST(BufferPoolDeathTest, AppendRegionsExhaustingTheIndexSpaceTerminates)
{
    RegisteredBufferPool pool(65536, 40960);  // needs 3 table entries

    std::vector<iovec> table(65534, iovec{nullptr, 1});  // room for one more entry only
    EXPECT_DEATH((void)pool.append_regions(table), "index space exhausted");
}

// ===========================================================================
// Reserved-wake protocol, standalone (fake wake callback, no Context)
// ===========================================================================

namespace
{
// set_wake takes a capture-less function pointer, so parked handles are
// collected through a file-scope vector. Sequential tests only.
std::vector<std::coroutine_handle<>> g_woken_handles;

void collect_wake(void*, std::coroutine_handle<> h) noexcept
{
    g_woken_handles.push_back(h);
}

void reset_wakes()
{
    g_woken_handles.clear();
}

// Parks on acquire() and keeps the lease in its frame until the Task is
// destroyed; the caller drives the waits manually.
Task<BufferLease> lease_waiter(RegisteredBufferPool& p, int* completed = nullptr)
{
    auto r = co_await p.acquire();
    if (completed != nullptr)
    {
        *completed = r.has_value() ? 1 : -1;
    }
    // The whole Result moves through: an empty lease would read as a
    // VALUE (a falsy one) and swallow the cancellation error.
    co_return std::move(r);
}

// Releases its lease as soon as it acquires: the release is what chains
// the wake to the next waiter.
Task<> release_waiter(RegisteredBufferPool& p, int* status)
{
    auto r = co_await p.acquire();
    if (r.has_value())
    {
        *status = 1;
        r->reset();
    }
    else
    {
        *status = -1;
    }
    co_return {};
}
}  // namespace

TEST(BufferPoolProtocolTest, WokenWaiterIsNotStolen)
{
    reset_wakes();
    RegisteredBufferPool pool{4096, 1};
    pool.set_wake(nullptr, collect_wake);

    auto lease = pool.try_acquire();  // exhaust the single slot

    auto waiter = lease_waiter(pool);
    waiter.await_suspend(std::noop_coroutine()).resume();  // parks
    EXPECT_EQ(pool.waiting(), 1u);
    EXPECT_EQ(g_woken_handles.size(), 0u);

    lease.reset();  // release goes through the callback, never inline
    ASSERT_EQ(g_woken_handles.size(), 1u);
    EXPECT_FALSE(pool.try_acquire());  // the freed slot is reserved for the waiter

    g_woken_handles[0].resume();
    auto got = waiter.await_resume();
    ASSERT_TRUE(got.has_value());  // the reservation guaranteed it
    EXPECT_EQ(pool.waiting(), 0u);
}

TEST(BufferPoolProtocolTest, ThreeWaitersWakeFifoChained)
{
    reset_wakes();
    RegisteredBufferPool pool{4096, 1};
    pool.set_wake(nullptr, collect_wake);

    auto lease = pool.try_acquire();

    int s1 = 0, s2 = 0, s3 = 0;
    auto w1 = release_waiter(pool, &s1);
    auto w2 = release_waiter(pool, &s2);
    auto w3 = release_waiter(pool, &s3);
    w1.await_suspend(std::noop_coroutine()).resume();
    w2.await_suspend(std::noop_coroutine()).resume();
    w3.await_suspend(std::noop_coroutine()).resume();
    ASSERT_EQ(pool.waiting(), 3u);
    EXPECT_EQ(g_woken_handles.size(), 0u);

    lease.reset();  // wakes w1 only
    ASSERT_EQ(g_woken_handles.size(), 1u);
    EXPECT_FALSE(pool.try_acquire());  // the slot is reserved for w1

    g_woken_handles[0].resume();  // w1 acquires then releases -> wakes w2
    EXPECT_EQ(s1, 1);
    ASSERT_EQ(g_woken_handles.size(), 2u);

    g_woken_handles[1].resume();  // w2 -> wakes w3
    EXPECT_EQ(s2, 1);
    ASSERT_EQ(g_woken_handles.size(), 3u);

    g_woken_handles[2].resume();
    EXPECT_EQ(s3, 1);
    EXPECT_EQ(pool.waiting(), 0u);
}

// close() with a still-parked waiter: the wake goes through the callback
// and the waiter resumes into the closed branch -- operation_canceled.
TEST(BufferPoolProtocolTest, CloseCancelsAParkedWaiter)
{
    reset_wakes();
    RegisteredBufferPool pool{4096, 1};
    pool.set_wake(nullptr, collect_wake);

    auto lease = pool.try_acquire();
    int completed = 0;
    auto waiter = lease_waiter(pool, &completed);
    waiter.await_suspend(std::noop_coroutine()).resume();
    ASSERT_EQ(pool.waiting(), 1u);

    pool.close();
    ASSERT_EQ(g_woken_handles.size(), 1u);
    g_woken_handles[0].resume();

    auto got = waiter.await_resume();
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(got.error().value(), static_cast<int>(std::errc::operation_canceled));
    EXPECT_EQ(got.error().context(), "pool closed");
}

// The race shape: release() wakes W (handle already popped and reserved),
// then close() runs before W resumes. W must still see cancellation, not a
// phantom reservation.
TEST(BufferPoolProtocolTest, WokenThenClosedStillCancels)
{
    reset_wakes();
    RegisteredBufferPool pool{4096, 1};
    pool.set_wake(nullptr, collect_wake);

    auto lease = pool.try_acquire();
    int completed = 0;
    auto waiter = lease_waiter(pool, &completed);
    waiter.await_suspend(std::noop_coroutine()).resume();

    lease.reset();  // wake queued
    ASSERT_EQ(g_woken_handles.size(), 1u);
    pool.close();  // before the waiter resumes

    g_woken_handles[0].resume();
    auto got = waiter.await_resume();
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(got.error().value(), static_cast<int>(std::errc::operation_canceled));
    EXPECT_EQ(got.error().context(), "pool closed");
}

TEST(BufferPoolProtocolTest, CloseIsIdempotentAndLeasesStayReleasable)
{
    reset_wakes();
    RegisteredBufferPool pool{4096, 2};
    pool.set_wake(nullptr, collect_wake);

    auto lease = pool.try_acquire();
    ASSERT_TRUE(lease);

    pool.close();
    EXPECT_TRUE(pool.closed());
    pool.close();  // second call: no waiter drain, no abort
    EXPECT_TRUE(pool.closed());

    EXPECT_FALSE(pool.try_acquire());  // closed pools hand out nothing

    lease.reset();  // outstanding leases stay releasable after close
}

TEST(BufferPoolProtocolTest, AcquireAfterCloseFailsWithoutParking)
{
    reset_wakes();
    RegisteredBufferPool pool{4096, 1};
    pool.set_wake(nullptr, collect_wake);
    pool.close();

    int completed = 0;
    auto waiter = lease_waiter(pool, &completed);
    waiter.await_suspend(std::noop_coroutine()).resume();  // await_ready fast-paths on closed_

    auto got = waiter.await_resume();
    EXPECT_FALSE(got.has_value());
    EXPECT_EQ(got.error().context(), "pool closed");
    EXPECT_EQ(pool.waiting(), 0u);
    EXPECT_EQ(g_woken_handles.size(), 0u);
}

// Invariant 6: parking where nobody could ever wake you is a bug, caught at
// the park site -- not a coroutine that silently never resumes.
TEST(BufferPoolProtocolDeathTest, ParkingWithNoWakeCallbackTerminates)
{
    RegisteredBufferPool pool{4096, 1};
    auto lease = pool.try_acquire();
    ASSERT_TRUE(lease);

    EXPECT_DEATH(
        {
            auto waiter = lease_waiter(pool);
            waiter.await_suspend(std::noop_coroutine()).resume();
        },
        "no wake callback");
}

// acquire_buffer() with no pool configured must fail loudly, not hand back
// an awaiter over a null optional.
TEST(BufferPoolDeathTest, AcquireBufferWithoutPoolTerminates)
{
    io::IoOptions opts;
    opts.entries = 64;
    EXPECT_DEATH(
        {
            io::Context ctx(0, nullptr, opts);
            auto waiter = [](io::Context& c) -> Task<BufferLease>
            {
                auto r = co_await c.acquire_buffer();
                co_return r.has_value() ? std::move(*r) : BufferLease{};
            }(ctx);
            waiter.await_suspend(std::noop_coroutine()).resume();
        },
        "no buffer pool configured");
}

// ===========================================================================
// Fixed-op validation
// ===========================================================================

namespace
{
Task<> read_fixed_expect(io::Context& io, const io::UniqueFd& fd, BufferLease& lease, const size_t len,
                         const size_t buffer_offset, Result<int>& out)
{
    out = co_await async_read_fixed(io, fd, lease, len, buffer_offset, 0);
    co_return {};
}

Task<> write_fixed_len(io::Context& io, const io::UniqueFd& fd, const size_t len, Result<int>& out)
{
    auto lease = co_await io.acquire_buffer();
    EXPECT_TRUE(lease.has_value());
    std::memcpy(lease->data(), "abcdef", 6);
    out = co_await async_write_fixed(io, fd, *lease, len, 0, 0);
    co_return {};
}

Task<> write_fixed_expect(io::Context& io, const io::UniqueFd& fd, BufferLease& lease, const size_t len,
                          const size_t buffer_offset, Result<int>& out)
{
    out = co_await async_write_fixed(io, fd, lease, len, buffer_offset, 0);
    co_return {};
}
}  // namespace

// A zero length or an offset at the end of the slot pre-fails EINVAL at
// call time: await_ready() is true, no SQE is ever submitted, and run()
// still quiesces (inflight_ never moved).
TEST(BufferPoolTest, ReadFixedBadRangePrefailsEinval)
{
    test::TempDir dir;
    const auto file_path = dir.path() / "fixed_prefail.bin";
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::UniqueFd fd = open_temp_file(file_path);
    ASSERT_TRUE(fd.valid());

    auto lease = ctx.try_acquire_buffer();
    ASSERT_TRUE(lease);

    Result<int> zero_len{0};
    ctx.spawn(read_fixed_expect(ctx, fd, lease, 0, 0, zero_len));
    ctx.run();
    ASSERT_FALSE(zero_len.has_value());
    EXPECT_EQ(zero_len.error().value(), EINVAL);

    // offset == slot size: nothing readable inside the slot.
    Result<int> bad_offset{0};
    ctx.spawn(read_fixed_expect(ctx, fd, lease, 16, 4096, bad_offset));
    ctx.run();
    ASSERT_FALSE(bad_offset.has_value());
    EXPECT_EQ(bad_offset.error().value(), EINVAL);
}

// write_fixed mirrors read_fixed's pre-fail: a zero length or an offset at
// the end of the slot resumes with EINVAL without ever touching the ring
// (the OOB-pointer bug this pins used to abort in registration_index
// instead).
TEST(BufferPoolTest, WriteFixedBadRangePrefailsEinval)
{
    test::TempDir dir;
    const auto file_path = dir.path() / "fixed_w_prefail.bin";
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::UniqueFd fd = open_temp_file(file_path);
    ASSERT_TRUE(fd.valid());

    auto lease = ctx.try_acquire_buffer();
    ASSERT_TRUE(lease);

    Result<int> bad_offset{0};
    ctx.spawn(write_fixed_expect(ctx, fd, lease, 16, 4096, bad_offset));
    ctx.run();
    ASSERT_FALSE(bad_offset.has_value());
    EXPECT_EQ(bad_offset.error().value(), EINVAL);

    Result<int> zero_len{0};
    ctx.spawn(write_fixed_expect(ctx, fd, lease, 0, 0, zero_len));
    ctx.run();
    ASSERT_FALSE(zero_len.has_value());
    EXPECT_EQ(zero_len.error().value(), EINVAL);
}

// A len beyond the remaining slot is clamped to the slot: a SHORT write of
// exactly slot_size bytes, which the caller loops on.
TEST(BufferPoolTest, WriteFixedLenClampsToTheSlot)
{
    test::TempDir dir;
    const auto file_path = dir.path() / "fixed_clamp.bin";
    io::IoOptions opts;
    opts.entries = 64;
    io::Context ctx(0, nullptr, opts, BufferPoolConfig{.slot_size = 4096, .slots = 4});
    io::UniqueFd fd = open_temp_file(file_path);
    ASSERT_TRUE(fd.valid());

    Result<int> out{0};
    ctx.spawn(write_fixed_len(ctx, fd, 10000, out));
    ctx.run();

    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 4096);

    struct stat st{};
    ASSERT_EQ(::fstat(fd.get(), &st), 0);
    EXPECT_EQ(st.st_size, 4096);
}
