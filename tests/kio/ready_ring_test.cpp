#include "kio/detail/trivial_ring_buffer.h"

#include <cstdint>
#include <deque>
#include <random>

#include <gtest/gtest.h>

using kio::detail::TrivialRingBuffer;

// Test-only access to the monotonic counters, granted by a friend
// declaration in TrivialRingBuffer: the 2^64-wrap property can only be
// exercised by parking head_/tail_ just below UINT64_MAX.
namespace kio::detail
{
struct ReadyRingTestPeer
{
    template <typename T>
    static void set_counters(TrivialRingBuffer<T>& r, std::uint64_t head, std::uint64_t tail)
    {
        r.head_ = head;
        r.tail_ = tail;
    }

    template <typename T>
    static std::uint64_t head(const TrivialRingBuffer<T>& r)
    {
        return r.head_;
    }

    template <typename T>
    static std::uint64_t tail(const TrivialRingBuffer<T>& r)
    {
        return r.tail_;
    }
};
}  // namespace kio::detail

// ===========================================================================
// PART 1: FUNCTIONAL TESTS (The "Happy Path" & Edge Cases)
// ===========================================================================

TEST(TrivialRingBufferTest, FifoAcrossWraparoundWithoutGrow)
{
    TrivialRingBuffer<std::uint64_t> r(4);

    r.push(1);
    r.push(2);
    r.push(3);
    r.push(4);

    std::uint64_t buf[4];
    ASSERT_EQ(r.pop_batch(buf, 2), 2u);
    EXPECT_EQ(buf[0], 1u);
    EXPECT_EQ(buf[1], 2u);

    // Wrap around
    r.push(5);
    r.push(6);

    ASSERT_EQ(r.pop_batch(buf, 4), 4u);
    EXPECT_EQ(buf[0], 3u);
    EXPECT_EQ(buf[1], 4u);
    EXPECT_EQ(buf[2], 5u);
    EXPECT_EQ(buf[3], 6u);
    EXPECT_TRUE(r.empty());
}

TEST(TrivialRingBufferTest, GrowPreservesFifoOrder)
{
    TrivialRingBuffer<std::uint64_t> r(4);
    for (std::uint64_t i = 1; i <= 10; ++i)
        r.push(i);  // Forces 4 -> 8 -> 16

    std::uint64_t buf[3];
    std::uint64_t next = 1;
    while (!r.empty())
    {
        const std::uint64_t n = r.pop_batch(buf, 3);
        ASSERT_GT(n, 0u);
        for (std::uint64_t i = 0; i < n; ++i)
            EXPECT_EQ(buf[i], next++);
    }
    EXPECT_EQ(next, 11u);
}

TEST(TrivialRingBufferTest, PopBatchPartialThenEmpty)
{
    TrivialRingBuffer<std::uint64_t> r(8);
    for (std::uint64_t i = 1; i <= 5; ++i)
        r.push(i);

    std::uint64_t buf[3];
    ASSERT_EQ(r.pop_batch(buf, 3), 3u);  // Full batch
    ASSERT_EQ(r.pop_batch(buf, 3), 2u);  // Partial batch
    ASSERT_EQ(r.pop_batch(buf, 3), 0u);  // Empty
}

TEST(TrivialRingBufferTest, PopBatchMaxZeroAndMaxBeyondSize)
{
    TrivialRingBuffer<std::uint64_t> r(8);
    r.push(1);
    r.push(2);

    std::uint64_t buf[4];
    EXPECT_EQ(r.pop_batch(buf, 0), 0u);  // max == 0 copies nothing
    EXPECT_EQ(r.size(), 2u);

    EXPECT_EQ(r.pop_batch(buf, 100), 2u);  // max > size drains exactly size
    EXPECT_EQ(buf[0], 1u);
    EXPECT_EQ(buf[1], 2u);
    EXPECT_TRUE(r.empty());

    EXPECT_EQ(r.pop_batch(buf, 4), 0u);  // and stays empty
}

// The case random fuzzing might miss: grow() while the live region wraps
// the physical array. cap 16: fill 16, pop 10, push 10 (tail wraps around
// to physical index 10, ring exactly full), then one more push forces a
// grow whose two-leg copy must rebase head_ to 0 without losing order.
TEST(TrivialRingBufferTest, GrowWhileWrappedPreservesFifoOrder)
{
    TrivialRingBuffer<std::uint64_t> r(16);

    for (std::uint64_t i = 0; i < 16; ++i)
        r.push(i);
    std::uint64_t buf[16];
    ASSERT_EQ(r.pop_batch(buf, 10), 10u);

    for (std::uint64_t i = 16; i < 26; ++i)
        r.push(i);  // physical wrap
    ASSERT_EQ(r.size(), 16u);

    r.push(26);  // ring full AND wrapped: grow takes the two-leg path
    ASSERT_EQ(r.size(), 17u);

    std::uint64_t out[17];
    ASSERT_EQ(r.pop_batch(out, 17), 17u);
    for (std::uint64_t i = 0; i < 17; ++i)
    {
        EXPECT_EQ(out[i], 10u + i) << "FIFO order broken across wrapped grow at " << i;
    }
}

// The property the header comment claims: monotonic unsigned counters make
// size() = tail_ - head_ correct even when BOTH wrap past 2^64. The
// counters are parked just below UINT64_MAX so the pushes cross the
// boundary mid-test -- physical indices stay correct through the wrap
// because masking a wrapped counter is still a valid index.
TEST(TrivialRingBufferTest, CountersWrapPastTwoToThe64)
{
    TrivialRingBuffer<std::uint64_t> r(4);
    constexpr std::uint64_t kMax = ~std::uint64_t{0};
    kio::detail::ReadyRingTestPeer::set_counters(r, kMax - 3, kMax - 3);
    EXPECT_EQ(r.size(), 0u);

    for (std::uint64_t v = 1; v <= 4; ++v)
    {
        r.push(v);  // tail_ wraps past 2^64 on the third push
    }
    ASSERT_EQ(r.size(), 4u);

    std::uint64_t out[4];
    ASSERT_EQ(r.pop_batch(out, 4), 4u);
    for (std::uint64_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(out[i], i + 1) << "FIFO broken across the 2^64 wrap at " << i;
    }
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0u);
}

// grow() rebases the counters to (head_ = 0, tail_ = size): after the swap
// the logical order starts at physical index 0.
TEST(TrivialRingBufferTest, GrowRebasesCountersToZero)
{
    TrivialRingBuffer<std::uint64_t> r(4);
    for (std::uint64_t i = 1; i <= 5; ++i)
    {
        r.push(i);  // the fifth push grows 4 -> 8
    }

    EXPECT_EQ(kio::detail::ReadyRingTestPeer::head(r), 0u);
    EXPECT_EQ(kio::detail::ReadyRingTestPeer::tail(r), 5u);
    EXPECT_EQ(r.size(), 5u);

    std::uint64_t out[5];
    ASSERT_EQ(r.pop_batch(out, 5), 5u);
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(out[i], i + 1);
    }
}

// Differential fuzz against a std::deque model: random pushes and
// pop_batch(max in [0, 300]) with a small initial capacity so push-driven
// growth, both memcpy legs, and every wrap offset occur in combination.
TEST(TrivialRingBufferTest, DifferentialFuzzAgainstDequeModel)
{
    std::mt19937_64 rng(20261013u);  // fixed seed: reproducible failures

    TrivialRingBuffer<std::uint64_t> r(8);
    std::deque<std::uint64_t> model;

    std::uint64_t next_value = 0;
    std::uint64_t next_expected = 0;
    std::vector<std::uint64_t> out(300);

    for (int step = 0; step < 1'000'000; ++step)
    {
        if (rng() % 2 == 0 || model.empty())
        {
            r.push(next_value);
            model.push_back(next_value);
            ++next_value;
        }
        else
        {
            const std::uint64_t max = rng() % 301;
            const std::uint64_t k = r.pop_batch(out.data(), max);
            ASSERT_LE(k, max);
            ASSERT_EQ(k, std::min<std::uint64_t>(max, model.size()));
            for (std::uint64_t i = 0; i < k; ++i)
            {
                ASSERT_EQ(out[i], next_expected) << "divergence at step " << step;
                ++next_expected;
                model.pop_front();
            }
        }
        ASSERT_EQ(r.size(), model.size()) << "size divergence at step " << step;
    }

    // Drain both fully and compare the tails.
    while (!model.empty())
    {
        const std::uint64_t k = r.pop_batch(out.data(), 300);
        ASSERT_GT(k, 0u);
        for (std::uint64_t i = 0; i < k; ++i)
        {
            ASSERT_EQ(out[i], next_expected++);
            model.pop_front();
        }
    }
    EXPECT_TRUE(r.empty());
}

// ===========================================================================
// PART 2: DEATH TESTS (Testing the CHECK() fatal assertions)
// ===========================================================================

// 1. Name the suite with "DeathTest" so gtest runs it first and isolates it.
class TrivialRingBufferDeathTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 2. Set the threadsafe flag.
        // (Can also be done globally in main(), but doing it here ensures
        // it applies even if this specific test file is run in isolation).
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
};

// 3. Name the test case with "DeathTest"
TEST_F(TrivialRingBufferDeathTest, RejectsZeroCapacity)
{
    // 4. Construct INSIDE the macro.
    // 5. Provide a regex that matches the CHECK failure message.
    EXPECT_DEATH(
        { TrivialRingBuffer<std::uint64_t> bad_ring(0); },
        "positive power of two"  // Matches: "Capacity must be a positive power of two..."
    );
}

TEST_F(TrivialRingBufferDeathTest, RejectsNonPowerOfTwoCapacity)
{
    EXPECT_DEATH({ TrivialRingBuffer<std::uint64_t> bad_ring(3); }, "positive power of two");
}

TEST_F(TrivialRingBufferDeathTest, RefusesToGrowPastMaxCapacity)
{
    EXPECT_DEATH(
        {
            // Start at max capacity to avoid massive memory allocation during test setup
            constexpr auto kMax = TrivialRingBuffer<std::uint64_t>::kMaxCapacity;
            TrivialRingBuffer<std::uint64_t> r(kMax);

            // Fill it to the absolute brim
            for (std::uint64_t i = 0; i < kMax; ++i)
            {
                r.push(i);
            }

            // This push forces a grow(), which should trigger the CHECK and abort
            r.push(999);
        },
        "refuses to grow past"  // Matches: "TrivialRingBuffer refuses to grow past..."
    );
}
