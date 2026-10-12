#include "kio/detail/trivial_ring_buffer.h"

#include <cstdint>
#include <gtest/gtest.h>


using kio::detail::TrivialRingBuffer;

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
    for (std::uint64_t i = 1; i <= 10; ++i) r.push(i); // Forces 4 -> 8 -> 16

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
    for (std::uint64_t i = 1; i <= 5; ++i) r.push(i);

    std::uint64_t buf[3];
    ASSERT_EQ(r.pop_batch(buf, 3), 3u); // Full batch
    ASSERT_EQ(r.pop_batch(buf, 3), 2u); // Partial batch
    ASSERT_EQ(r.pop_batch(buf, 3), 0u); // Empty
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
        {
        TrivialRingBuffer<std::uint64_t> bad_ring(0);
        },
        "positive power of two" // Matches: "Capacity must be a positive power of two..."
    );
}

TEST_F(TrivialRingBufferDeathTest, RejectsNonPowerOfTwoCapacity)
{
    EXPECT_DEATH(
        {
        TrivialRingBuffer<std::uint64_t> bad_ring(3);
        },
        "positive power of two"
    );
}

TEST_F(TrivialRingBufferDeathTest, RefusesToGrowPastMaxCapacity)
{
    EXPECT_DEATH(
        {
        // Start at max capacity to avoid massive memory allocation during test setup
        constexpr auto kMax = TrivialRingBuffer<std::uint64_t>::kMaxCapacity;
        TrivialRingBuffer<std::uint64_t> r(kMax);

        // Fill it to the absolute brim
        for (std::uint64_t i = 0; i < kMax; ++i) {
        r.push(i);
        }

        // This push forces a grow(), which should trigger the CHECK and abort
        r.push(999);
        },
        "refuses to grow past" // Matches: "TrivialRingBuffer refuses to grow past..."
    );
}
