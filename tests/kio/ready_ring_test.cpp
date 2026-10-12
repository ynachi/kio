#include "kio/detail/ready_ring.h"

#include <coroutine>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using kio::detail::ReadyRing;

namespace
{
// Fake handles carry a tag in the address bits and are NEVER resumed or
// destroyed -- there is no frame behind them. ReadyRing is tested here as
// pure storage: only push/pop_batch/size/empty are exercised.
std::coroutine_handle<> fake(const std::uint64_t v)
{
    return std::coroutine_handle<>::from_address(reinterpret_cast<void*>(static_cast<std::uintptr_t>(v)));
}

std::uint64_t tag(const std::coroutine_handle<> h)
{
    return reinterpret_cast<std::uintptr_t>(h.address());
}
}  // namespace

TEST(ReadyRingTest, FifoAcrossWraparoundWithoutGrow)
{
    ReadyRing r(4);
    for (std::uint64_t i = 1; i <= 4; ++i)
    {
        r.push(fake(i));
    }
    std::coroutine_handle<> buf[4];
    ASSERT_EQ(r.pop_batch(buf, 2), 2u);
    EXPECT_EQ(tag(buf[0]), 1u);
    EXPECT_EQ(tag(buf[1]), 2u);
    // These two wrap onto the slots freed by the first pop.
    r.push(fake(5));
    r.push(fake(6));
    ASSERT_EQ(r.pop_batch(buf, 4), 4u);
    EXPECT_EQ(tag(buf[0]), 3u);
    EXPECT_EQ(tag(buf[1]), 4u);
    EXPECT_EQ(tag(buf[2]), 5u);
    EXPECT_EQ(tag(buf[3]), 6u);
    EXPECT_TRUE(r.empty());
}

TEST(ReadyRingTest, GrowPreservesFifoOrder)
{
    ReadyRing r(4);
    for (std::uint64_t i = 1; i <= 10; ++i)
    {
        r.push(fake(i));  // fills at 4, grows 4->8->16
    }
    EXPECT_EQ(r.size(), 10u);
    std::coroutine_handle<> buf[3];
    std::uint64_t next = 1;
    while (!r.empty())
    {
        const std::uint64_t n = r.pop_batch(buf, 3);
        ASSERT_GT(n, 0u);
        for (std::uint64_t i = 0; i < n; ++i)
        {
            EXPECT_EQ(tag(buf[i]), next);
            ++next;
        }
    }
    EXPECT_EQ(next, 11u);
}

TEST(ReadyRingTest, PopBatchPartialThenEmpty)
{
    ReadyRing r(8);
    for (std::uint64_t i = 1; i <= 5; ++i)
    {
        r.push(fake(i));
    }
    std::coroutine_handle<> buf[3];
    ASSERT_EQ(r.pop_batch(buf, 3), 3u);
    EXPECT_EQ(tag(buf[0]), 1u);
    EXPECT_EQ(tag(buf[1]), 2u);
    EXPECT_EQ(tag(buf[2]), 3u);
    ASSERT_EQ(r.pop_batch(buf, 3), 2u);  // short tail batch
    EXPECT_EQ(tag(buf[0]), 4u);
    EXPECT_EQ(tag(buf[1]), 5u);
    ASSERT_EQ(r.pop_batch(buf, 3), 0u);
}

TEST(ReadyRingTest, PushDuringDrainInterleaving)
{
    ReadyRing r(8);
    r.push(fake(1));
    r.push(fake(2));
    r.push(fake(3));
    std::coroutine_handle<> buf[3];
    ASSERT_EQ(r.pop_batch(buf, 2), 2u);
    EXPECT_EQ(tag(buf[0]), 1u);
    EXPECT_EQ(tag(buf[1]), 2u);
    // Interleave: the pushed handles continue the same FIFO order.
    r.push(fake(4));
    r.push(fake(5));
    ASSERT_EQ(r.pop_batch(buf, 3), 3u);
    EXPECT_EQ(tag(buf[0]), 3u);
    EXPECT_EQ(tag(buf[1]), 4u);
    EXPECT_EQ(tag(buf[2]), 5u);
    EXPECT_TRUE(r.empty());
}

// Documents that exactly kMaxCapacity entries fit without tripping the fence
// CHECK: the ring grows 1024 -> 1<<20, and only a push onto a FULL max ring
// would abort. Tags start at 1, not 0: fake(0) is a null-address handle and
// push() CHECK-rejects those (same contract as Context::post), so the fence
// here is exercised on capacity alone.
TEST(ReadyRingTest, GrowsToCapacityFence)
{
    ReadyRing r{};
    constexpr std::uint64_t kMax = 1ULL << 20;
    for (std::uint64_t i = 0; i < kMax; ++i)
    {
        r.push(fake(i + 1));
    }
    EXPECT_EQ(r.size(), kMax);

    std::coroutine_handle<> head[4];
    ASSERT_EQ(r.pop_batch(head, 4), 4u);
    EXPECT_EQ(tag(head[0]), 1u);
    EXPECT_EQ(tag(head[1]), 2u);
    EXPECT_EQ(tag(head[2]), 3u);
    EXPECT_EQ(tag(head[3]), 4u);

    std::vector<std::coroutine_handle<>> buf(70000);
    std::uint64_t next = 5;
    while (!r.empty())
    {
        const std::uint64_t n = r.pop_batch(buf.data(), buf.size());
        ASSERT_GT(n, 0u);
        // Each chunk's first tag continues the global sequence.
        EXPECT_EQ(tag(buf[0]), next);
        next += n;
    }
    EXPECT_EQ(next, kMax + 1);
    EXPECT_TRUE(r.empty());
}
