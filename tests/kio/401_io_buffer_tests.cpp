#include "kio/kio.hpp"

#include <cstring>
#include <random>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

using namespace kio;

class IoBufferTest : public ::testing::Test
{
protected:
    IoBuffer buf{1024};
};

// --- Basic Functionality Tests ---

TEST_F(IoBufferTest, InitialState)
{
    EXPECT_TRUE(buf.Empty());
    EXPECT_EQ(buf.ReadableBytes(), 0);
    EXPECT_EQ(buf.StagedBytes(), 0);
    EXPECT_GE(buf.Capacity(), 1024);
}

TEST_F(IoBufferTest, AppendAndRead)
{
    std::string_view data = "hello world";
    buf.Append(data);

    // Data is staged but not yet readable if we haven't committed
    // Wait, the provided IoBuffer::Append calls write_ += size, and Commit()
    // in the source actually handles commit_ = write_.
    // In the provided code, Append actually advances write_, but not commit_.
    // Let's verify the "Staged" vs "Readable" logic.

    EXPECT_EQ(buf.StagedBytes(), data.size());
    EXPECT_EQ(buf.ReadableBytes(), 0);

    buf.Commit();  // Move staged to readable

    EXPECT_EQ(buf.ReadableBytes(), data.size());
    auto span = buf.ReadableSpan();
    EXPECT_EQ(std::string_view(span.data(), span.size()), data);
}

TEST_F(IoBufferTest, ConsumeData)
{
    buf.Append("1234567890");
    buf.Commit();

    size_t consumed = buf.Consume(4);
    EXPECT_EQ(consumed, 4);
    EXPECT_EQ(buf.ReadableBytes(), 6);

    auto span = buf.ReadableSpan();
    EXPECT_EQ(std::string_view(span.data(), span.size()), "567890");
}

TEST_F(IoBufferTest, ClearResetsIndices)
{
    buf.Append("test");
    buf.Commit();
    buf.Clear();
    EXPECT_EQ(buf.ReadableBytes(), 0);
    EXPECT_EQ(buf.StagedBytes(), 0);
    EXPECT_TRUE(buf.Empty());
}

// --- Growth and Compaction Tests ---

TEST_F(IoBufferTest, AutomaticGrowth)
{
    size_t initial_cap = buf.Capacity();
    std::string large_data(initial_cap + 100, 'a');

    buf.Append(large_data);
    EXPECT_GT(buf.Capacity(), initial_cap);
    buf.Commit();
    EXPECT_EQ(buf.ReadableBytes(), large_data.size());
}

TEST_F(IoBufferTest, CompactionTrigger)
{
    // Fill buffer partially
    std::string chunk(600, 'x');
    buf.Append(chunk);
    buf.Commit();

    // Consume most of it to move read_ pointer forward
    buf.Consume(500);

    // Now readable is 100, read_ index is 500.
    // kCompactThreshold is 1024 in your code.
    // Let's force a scenario where WritableBytes() is low but compaction helps.

    // To trigger compaction: WritableBytes() < additional AND read_ >= kCompactThreshold
    // Since kCompactThreshold is 1024, let's move read_ further.
    buf.Clear();
    std::string filler(1500, 'y');
    buf.Append(filler);
    buf.Commit();
    buf.Consume(1200);  // read_ is now 1200, which is > 1024

    size_t prev_write = buf.ReadableBytesSpan().data() != nullptr ? 0 : 0;  // Dummy

    // This should trigger compaction because we need more space than currently at the end
    buf.EnsureWritableBytes(buf.Capacity() - 100);

    // After compaction, read_ should be 0
    // We check this indirectly: the data should still be correct
    buf.Commit();
    auto span = buf.ReadableSpan();
    EXPECT_EQ(span.size(), 300);
    EXPECT_EQ(span[0], 'y');
}

// --- Protocol Helper Tests ---

TEST_F(IoBufferTest, FindCrlfCorrectness)
{
    buf.Append("First Line\r\nSecond Line\r\n");
    buf.Commit();

    auto pos = buf.FindCrlf();
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(*pos, 10);  // Index of \r

    buf.Consume(*pos + 2);  // Consume "First Line\r\n"

    auto pos2 = buf.FindCrlf();
    ASSERT_TRUE(pos2.has_value());
    EXPECT_EQ(*pos2, 11);  // Index of \r in "Second Line\r\n"
}

// --- Stress Test ---

TEST(IoBufferStress, RandomOperations)
{
    IoBuffer buffer(1024);
    std::string mirror;  // To verify content
    std::mt19937 rng(42);

    for (int i = 0; i < 1000; ++i)
    {
        int op = std::uniform_int_distribution(0, 2)(rng);

        if (op == 0)
        {  // Append
            int len = std::uniform_int_distribution(1, 500)(rng);
            std::string chunk(len, static_cast<char>('a' + (i % 26)));
            buffer.Append(chunk);
            buffer.Commit();
            mirror += chunk;
        }
        else if (op == 1 && !mirror.empty())
        {  // Consume
            int len = std::uniform_int_distribution<>(1, mirror.size())(rng);
            buffer.Consume(len);
            mirror.erase(0, len);
        }
        else
        {  // Compact/Reserve check
            buffer.EnsureWritableBytes(std::uniform_int_distribution(1, 2000)(rng));
        }

        // Validation
        ASSERT_EQ(buffer.ReadableBytes(), mirror.size());
        auto span = buffer.ReadableSpan();
        std::string_view buf_view(span.data(), span.size());
        ASSERT_EQ(buf_view, mirror);
    }
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}