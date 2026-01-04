//
// Created by Yao ACHI on 23/11/2025.
//

#include "../../core/bytes_mut.h"

#include <gtest/gtest.h>

using kio::BytesMut;

TEST(BytesMutTest, InitialState)
{
    const BytesMut buf;

    EXPECT_EQ(buf.remaining(), 0u);
    EXPECT_EQ(buf.writable(), 0u);
    EXPECT_TRUE(buf.is_empty());
    EXPECT_EQ(buf.capacity(), 0u);
}

TEST(BytesMutTest, ReserveAllocatesSpace)
{
    BytesMut buf;

    buf.reserve(32);
    EXPECT_GE(buf.capacity(), 32u);
    EXPECT_GE(buf.writable(), 32u);
    EXPECT_EQ(buf.remaining(), 0u);
}

TEST(BytesMutTest, WritableSpanAndCommit)
{
    BytesMut buf;
    buf.reserve(16);

    auto w = buf.writable_span();
    ASSERT_GE(w.size(), 16u);

    const std::string test = "hello123";
    memcpy(w.data(), test.data(), test.size());
    buf.commit_write(test.size());

    EXPECT_EQ(buf.remaining(), test.size());

    auto r = buf.readable_span();
    EXPECT_EQ(std::string(r.begin(), r.end()), test);
}

TEST(BytesMutTest, AdvanceConsumesData)
{
    BytesMut buf;
    buf.reserve(16);

    {
        auto w = buf.writable_span();
        memcpy(w.data(), "abcdef", 6);
        buf.commit_write(6);
    }

    EXPECT_EQ(buf.remaining(), 6u);
    buf.advance(3);

    EXPECT_EQ(buf.remaining(), 3u);

    auto r = buf.readable_span();
    EXPECT_EQ(std::string(r.begin(), r.end()), "def");
}

TEST(BytesMutTest, AdvanceThrowsOnOverflow)
{
    BytesMut buf;
    buf.reserve(8);

    auto w = buf.writable_span();
    memcpy(w.data(), "hi", 2);
    buf.commit_write(2);

    EXPECT_THROW(buf.advance(5), std::out_of_range);
}

TEST(BytesMutTest, ExtendFromSlice)
{
    BytesMut buf;

    const char* msg = "hello world";
    buf.extend_from_slice({msg, 11});

    EXPECT_EQ(buf.remaining(), 11u);

    auto r = buf.readable_span();
    EXPECT_EQ(std::string(r.begin(), r.end()), "hello world");
}

TEST(BytesMutTest, Peek)
{
    BytesMut buf;

    const char* msg = "0123456789";
    buf.extend_from_slice({msg, 10});

    auto first4 = buf.peek(4);
    EXPECT_EQ(std::string(first4.begin(), first4.end()), "0123");

    EXPECT_THROW(buf.peek(20), std::out_of_range);
}

TEST(BytesMutTest, CompactReclaimsSpace)
{
    BytesMut buf;
    buf.reserve(16);

    buf.extend_from_slice({"abcdefgh", 8});
    buf.advance(5);  // consumed "abcde"

    EXPECT_EQ(buf.remaining(), 3u);
    EXPECT_EQ(buf.consumed(), 5u);

    buf.compact();

    EXPECT_EQ(buf.remaining(), 3u);
    EXPECT_EQ(buf.consumed(), 0u);

    auto r = buf.readable_span();
    EXPECT_EQ(std::string(r.begin(), r.end()), "fgh");
}

TEST(BytesMutTest, ShouldCompactLogic)
{
    BytesMut buf;
    buf.reserve(16);

    buf.extend_from_slice({"ABCDEFGHIJKLMNOP", 16});
    buf.advance(8);

    EXPECT_TRUE(buf.should_compact());
}

TEST(BytesMutTest, GrowthPreservesData)
{
    BytesMut buf;
    buf.reserve(8);

    buf.extend_from_slice({"12345678", 8});
    buf.extend_from_slice({"ABCDEF", 6});  // triggers reserve grow

    auto r = buf.readable_span();
    EXPECT_EQ(std::string(r.begin(), r.end()), "12345678ABCDEF");
}

TEST(BytesMutTest, ClearResetsPointersButKeepsCapacity)
{
    BytesMut buf;
    buf.reserve(32);

    buf.extend_from_slice({"test", 4});
    buf.clear();

    EXPECT_TRUE(buf.is_empty());
    EXPECT_EQ(buf.remaining(), 0u);
    EXPECT_EQ(buf.writable(), buf.capacity());
}

TEST(BytesMutTest, CommitWriteThrowsOutOfRange)
{
    BytesMut buf;
    buf.reserve(8);

    EXPECT_THROW(buf.commit_write(100), std::out_of_range);
}

TEST(BytesMutTest, MultipleWritesAndReads)
{
    BytesMut buf;
    buf.reserve(32);

    buf.extend_from_slice({"abc", 3});
    buf.extend_from_slice({"12345", 5});
    buf.advance(3);  // consume "abc"

    auto r = buf.readable_span();
    EXPECT_EQ(std::string(r.begin(), r.end()), "12345");

    buf.extend_from_slice({"XYZ", 3});

    r = buf.readable_span();
    EXPECT_EQ(std::string(r.begin(), r.end()), "12345XYZ");
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
