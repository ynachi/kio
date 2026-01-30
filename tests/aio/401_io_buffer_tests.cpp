/**
 * io_buffer_test.cpp
 *
 * Unit tests for IoBuffer ring buffer implementation using Google Test.
 *
 * Compile:
 *   g++ -std=c++23 -O2 -o io_buffer_test io_buffer_test.cpp -lgtest -lgtest_main -pthread
 *
 * Run:
 *   ./io_buffer_test
 */

#include <cstring>
#include <string>
#include <vector>

#include "aio/io.hpp"
#include <gtest/gtest.h>

using namespace aio;

// =============================================================================
// Construction Tests
// =============================================================================

TEST(IoBufferConstruction, DefaultConstruction)
{
    IoBuffer buf(4096);
    EXPECT_EQ(buf.Capacity(), 4096u);
    EXPECT_EQ(buf.ReadableBytes(), 0u);
    EXPECT_EQ(buf.WritableBytes(), 4096u);
    EXPECT_TRUE(buf.IsEmpty());
    EXPECT_FALSE(buf.IsFull());
}

TEST(IoBufferConstruction, CapacityRoundsUpToPowerOfTwo)
{
    IoBuffer buf(1000);  // Should round up to 1024
    EXPECT_EQ(buf.Capacity(), 1024u);
    EXPECT_EQ(buf.Mask(), 1023u);
}

TEST(IoBufferConstruction, SmallCapacityUsesMinimum)
{
    IoBuffer buf(1);  // Should use minimum (64)
    EXPECT_EQ(buf.Capacity(), 64u);
}

// TEST(IoBufferConstruction, ConstructFromStringView)
// {
//     const char* data = "Hello, World!";
//     IoBuffer buf(std::string_view{data});
//
//     EXPECT_EQ(buf.ReadableBytes(), 13u);
//     EXPECT_EQ(buf.ToString(), "Hello, World!");
// }
//
// TEST(IoBufferConstruction, ConstructFromSpan)
// {
//     std::vector<char> data = {'T', 'e', 's', 't'};
//     IoBuffer buf(std::span<const char>{data});
//
//     EXPECT_EQ(buf.ReadableBytes(), 4u);
//     EXPECT_EQ(buf.ToString(), "Test");
// }
//
// // =============================================================================
// // Copy Tests
// // =============================================================================
//
// TEST(IoBufferCopy, CopyConstruction)
// {
//     IoBuffer buf1(256);
//     buf1.Append("Hello");
//
//     IoBuffer buf2(buf1);  // Copy
//
//     EXPECT_EQ(buf2.ReadableBytes(), 5u);
//     EXPECT_EQ(buf2.ToString(), "Hello");
//     EXPECT_EQ(buf2.Capacity(), buf1.Capacity());
//
//     // Modify original, copy should be unaffected
//     buf1.Append(" World");
//     EXPECT_EQ(buf1.ToString(), "Hello World");
//     EXPECT_EQ(buf2.ToString(), "Hello");  // Still "Hello"
// }
//
// TEST(IoBufferCopy, CopyAssignment)
// {
//     IoBuffer buf1(256);
//     buf1.Append("Original");
//
//     IoBuffer buf2(128);
//     buf2.Append("Replaced");
//
//     buf2 = buf1;  // Copy assign
//
//     EXPECT_EQ(buf2.ToString(), "Original");
// }
//
// TEST(IoBufferCopy, SelfAssignment)
// {
//     IoBuffer buf(256);
//     buf.Append("SelfTest");
//
//     buf = buf;  // Self-assignment
//
//     EXPECT_EQ(buf.ToString(), "SelfTest");
// }
//
// TEST(IoBufferCopy, CopyWrappedBuffer)
// {
//     IoBuffer buf1(64);  // Small buffer to force wrapping
//
//     // Fill most of buffer
//     std::string filler(50, 'x');
//     buf1.Append(filler);
//
//     // Consume some to advance read pointer
//     buf1.Consume(40);
//
//     // Write more to wrap around
//     buf1.Append("WRAPPED");
//
//     // Now copy the wrapped buffer
//     IoBuffer buf2(buf1);
//
//     EXPECT_EQ(buf1.ReadableBytes(), buf2.ReadableBytes());
//     EXPECT_EQ(buf1.ToString(), buf2.ToString());
// }
//
// // =============================================================================
// // Move Tests
// // =============================================================================
//
// TEST(IoBufferMove, MoveConstruction)
// {
//     IoBuffer buf1(256);
//     buf1.Append("MovedData");
//
//     IoBuffer buf2(std::move(buf1));
//
//     EXPECT_EQ(buf2.ToString(), "MovedData");
//     EXPECT_EQ(buf1.Capacity(), 0u);  // Moved-from state
//     EXPECT_TRUE(buf1.IsEmpty());
// }
//
// TEST(IoBufferMove, MoveAssignment)
// {
//     IoBuffer buf1(256);
//     buf1.Append("Source");
//
//     IoBuffer buf2(128);
//     buf2.Append("Target");
//
//     buf2 = std::move(buf1);
//
//     EXPECT_EQ(buf2.ToString(), "Source");
// }
//
// // =============================================================================
// // Basic Read/Write Tests
// // =============================================================================
//
// TEST(IoBufferReadWrite, AppendAndRead)
// {
//     IoBuffer buf(256);
//
//     buf.Append("Hello");
//     EXPECT_EQ(buf.ReadableBytes(), 5u);
//
//     buf.Append(", World!");
//     EXPECT_EQ(buf.ReadableBytes(), 13u);
//
//     EXPECT_EQ(buf.ToString(), "Hello, World!");
// }
//
// TEST(IoBufferReadWrite, ReadableSpan)
// {
//     IoBuffer buf(256);
//     buf.Append("TestData");
//
//     auto span = buf.ReadableSpan();
//     EXPECT_EQ(span.size(), 8u);
//     EXPECT_EQ(std::string_view(span.data(), span.size()), "TestData");
// }
//
// TEST(IoBufferReadWrite, Consume)
// {
//     IoBuffer buf(256);
//     buf.Append("Hello, World!");
//
//     buf.Consume(7);  // Consume "Hello, "
//     EXPECT_EQ(buf.ReadableBytes(), 6u);
//     EXPECT_EQ(buf.ToString(), "World!");
// }
//
// TEST(IoBufferReadWrite, TryConsume)
// {
//     IoBuffer buf(256);
//     buf.Append("Test");
//
//     size_t consumed = buf.TryConsume(100);  // More than available
//     EXPECT_EQ(consumed, 4u);
//     EXPECT_TRUE(buf.IsEmpty());
// }
//
// TEST(IoBufferReadWrite, Peek)
// {
//     IoBuffer buf(256);
//     buf.Append("HelloWorld");
//
//     auto peek5 = buf.Peek(5);
//     EXPECT_EQ(std::string_view(peek5.data(), peek5.size()), "Hello");
//
//     // Buffer unchanged
//     EXPECT_EQ(buf.ReadableBytes(), 10u);
// }
//
// TEST(IoBufferReadWrite, PeekAt)
// {
//     IoBuffer buf(256);
//     buf.Append("ABCDE");
//
//     EXPECT_EQ(buf.PeekAt(0), 'A');
//     EXPECT_EQ(buf.PeekAt(4), 'E');
//     EXPECT_FALSE(buf.PeekAt(5).has_value());  // Out of range
// }
//
// TEST(IoBufferReadWrite, ReadIntoBuffer)
// {
//     IoBuffer buf(256);
//     buf.Append("Source Data");
//
//     char dest[6] = {0};
//     size_t read = buf.Read({dest, 6});
//
//     EXPECT_EQ(read, 6u);
//     EXPECT_EQ(std::string(dest), "Source");
//     EXPECT_EQ(buf.ReadableBytes(), 5u);  // " Data" remains
// }
//
// TEST(IoBufferReadWrite, ReadAllAsString)
// {
//     IoBuffer buf(256);
//     buf.Append("Complete String");
//
//     std::string result = buf.ReadAllAsString();
//
//     EXPECT_EQ(result, "Complete String");
//     EXPECT_TRUE(buf.IsEmpty());
// }
//
// // =============================================================================
// // Wraparound Tests
// // =============================================================================
//
// TEST(IoBufferWraparound, WriteReadWraparound)
// {
//     IoBuffer buf(64);  // Small buffer
//
//     // Fill buffer partially
//     std::string data1(50, 'A');
//     buf.Append(data1);
//     EXPECT_EQ(buf.ReadableBytes(), 50u);
//
//     // Consume most of it
//     buf.Consume(45);
//     EXPECT_EQ(buf.ReadableBytes(), 5u);
//
//     // Now write more, should wrap
//     std::string data2(40, 'B');
//     buf.Append(data2);
//
//     // Read should handle wraparound
//     auto [s1, s2] = buf.ReadableSpans();
//     EXPECT_FALSE(s2.empty());  // Should have wrapped
//
//     EXPECT_EQ(buf.ReadableBytes(), 45u);
//
//     // Verify content
//     std::string result = buf.ToString();
//     EXPECT_EQ(result, std::string(5, 'A') + std::string(40, 'B'));
// }
//
// TEST(IoBufferWraparound, ReadableSpansWrapped)
// {
//     IoBuffer buf(64);
//
//     // Create wrapped state
//     buf.Append(std::string(50, 'X'));
//     buf.Consume(45);
//     buf.Append(std::string(30, 'Y'));
//
//     auto [s1, s2] = buf.ReadableSpans();
//
//     // First span should be at the end of physical buffer
//     EXPECT_FALSE(s1.empty());
//     // Second span should be at the beginning
//     EXPECT_FALSE(s2.empty());
//
//     // Total should match
//     EXPECT_EQ(s1.size() + s2.size(), buf.ReadableBytes());
// }
//
// TEST(IoBufferWraparound, WritableSpansWrapped)
// {
//     IoBuffer buf(64);
//
//     // Partially fill and consume to position write near end
//     buf.Append(std::string(60, 'X'));
//     buf.Consume(55);
//
//     // Now writable space wraps around
//     auto [s1, s2] = buf.WritableSpans();
//
//     // s1 is space at end, s2 is space at beginning
//     EXPECT_FALSE(s1.empty());
//     EXPECT_FALSE(s2.empty());
//
//     EXPECT_EQ(s1.size() + s2.size(), buf.WritableBytes());
// }
//
// // =============================================================================
// // Growth Tests
// // =============================================================================
//
// TEST(IoBufferGrowth, EnsureWritableGrows)
// {
//     IoBuffer buf(64);
//
//     buf.EnsureWritableBytes(100);
//
//     EXPECT_GE(buf.Capacity(), 100u);
//     EXPECT_GE(buf.WritableBytes(), 100u);
// }
//
// TEST(IoBufferGrowth, EnsureWritablePreservesData)
// {
//     IoBuffer buf(64);
//     buf.Append("Preserve This");
//
//     buf.EnsureWritableBytes(200);  // Force growth
//
//     EXPECT_EQ(buf.ToString(), "Preserve This");
// }
//
// TEST(IoBufferGrowth, MaxCapacityThrows)
// {
//     IoBuffer buf(64);
//
//     EXPECT_THROW(buf.EnsureWritableBytes(IoBuffer::kMaxCapacity + 1), std::length_error);
// }
//
// TEST(IoBufferGrowth, ReserveGrows)
// {
//     IoBuffer buf(64);
//
//     buf.Reserve(1024);
//
//     EXPECT_GE(buf.Capacity(), 1024u);
// }
//
// TEST(IoBufferGrowth, ReserveDoesNotShrink)
// {
//     IoBuffer buf(1024);
//
//     buf.Reserve(64);  // Smaller than current
//
//     EXPECT_EQ(buf.Capacity(), 1024u);  // Unchanged
// }
//
// TEST(IoBufferGrowth, TryAppendNoGrow)
// {
//     IoBuffer buf(64);
//
//     bool ok = buf.TryAppend(std::string(100, 'X'));  // Too big
//     EXPECT_FALSE(ok);
//     EXPECT_TRUE(buf.IsEmpty());  // Nothing appended
//
//     ok = buf.TryAppend("Small");  // Fits
//     EXPECT_TRUE(ok);
//     EXPECT_EQ(buf.ToString(), "Small");
// }
//
// TEST(IoBufferGrowth, ShrinkToFit)
// {
//     IoBuffer buf(64);
//     buf.Reserve(4096);  // Grow large
//     EXPECT_EQ(buf.Capacity(), 4096u);
//
//     buf.Append("tiny");
//     buf.ShrinkToFit();
//
//     EXPECT_LT(buf.Capacity(), 4096u);
//     EXPECT_EQ(buf.ToString(), "tiny");
// }
//
// // =============================================================================
// // Search Tests
// // =============================================================================
//
// TEST(IoBufferSearch, FindByte)
// {
//     IoBuffer buf(256);
//     buf.Append("Hello, World!");
//
//     auto pos = buf.Find(',');
//     ASSERT_TRUE(pos.has_value());
//     EXPECT_EQ(*pos, 5u);
//
//     auto not_found = buf.Find('Z');
//     EXPECT_FALSE(not_found.has_value());
// }
//
// TEST(IoBufferSearch, FindByteWrapped)
// {
//     IoBuffer buf(64);
//
//     // Create wrapped buffer with target in second segment
//     buf.Append(std::string(50, 'A'));
//     buf.Consume(45);
//     buf.Append("BCDEF");  // 'D' is in second segment
//
//     auto pos = buf.Find('D');
//     EXPECT_TRUE(pos.has_value());
// }
//
// TEST(IoBufferSearch, FindCRLF)
// {
//     IoBuffer buf(256);
//     buf.Append("HTTP/1.1 200 OK\r\n");
//
//     auto pos = buf.FindCRLF();
//     ASSERT_TRUE(pos.has_value());
//     EXPECT_EQ(*pos, 15u);  // Position of \r
// }
//
// TEST(IoBufferSearch, FindCRLFNotFound)
// {
//     IoBuffer buf(256);
//     buf.Append("No line ending here");
//
//     auto pos = buf.FindCRLF();
//     EXPECT_FALSE(pos.has_value());
// }
//
// TEST(IoBufferSearch, StartsWith)
// {
//     IoBuffer buf(256);
//     buf.Append("HTTP/1.1 200 OK");
//
//     EXPECT_TRUE(buf.StartsWith("HTTP"));
//     EXPECT_TRUE(buf.StartsWith("HTTP/1.1"));
//     EXPECT_FALSE(buf.StartsWith("HTTPS"));
//     EXPECT_FALSE(buf.StartsWith("This is longer than the buffer content!"));
// }
//
// TEST(IoBufferSearch, StartsWithEmpty)
// {
//     IoBuffer buf(256);
//     buf.Append("Data");
//
//     EXPECT_TRUE(buf.StartsWith(""));  // Empty prefix always matches
// }
//
// // =============================================================================
// // iovec Tests
// // =============================================================================
//
// TEST(IoBufferIovec, ReadableIovecs)
// {
//     IoBuffer buf(256);
//     buf.Append("TestData");
//
//     auto vecs = buf.ReadableIovecs();
//     EXPECT_EQ(vecs.size(), 1u);  // Contiguous
//     EXPECT_EQ(vecs[0].iov_len, 8u);
// }
//
// TEST(IoBufferIovec, ReadableIovecsWrapped)
// {
//     IoBuffer buf(64);
//
//     // Create wrapped state
//     buf.Append(std::string(50, 'X'));
//     buf.Consume(45);
//     buf.Append(std::string(30, 'Y'));
//
//     auto vecs = buf.ReadableIovecs();
//     EXPECT_EQ(vecs.size(), 2u);  // Two segments
//
//     size_t total = vecs[0].iov_len + vecs[1].iov_len;
//     EXPECT_EQ(total, buf.ReadableBytes());
// }
//
// TEST(IoBufferIovec, WritableIovecs)
// {
//     IoBuffer buf(64);
//
//     auto vecs = buf.WritableIovecs();
//     EXPECT_FALSE(vecs.empty());
//
//     size_t total = 0;
//     for (const auto& v : vecs)
//         total += v.iov_len;
//     EXPECT_EQ(total, buf.WritableBytes());
// }
//
// TEST(IoBufferIovec, WritableByteSpan)
// {
//     IoBuffer buf(256);
//
//     auto span = buf.WritableByteSpan();
//     EXPECT_EQ(span.size(), buf.WritableBytes());
//
//     // Write via byte span
//     span[0] = std::byte{'H'};
//     span[1] = std::byte{'i'};
//     buf.Commit(2);
//
//     EXPECT_EQ(buf.ToString(), "Hi");
// }
//
// // =============================================================================
// // Comparison Tests
// // =============================================================================
//
// TEST(IoBufferComparison, Equality)
// {
//     IoBuffer buf1(256);
//     IoBuffer buf2(512);  // Different capacity, same content
//
//     buf1.Append("Same");
//     buf2.Append("Same");
//
//     EXPECT_EQ(buf1, buf2);
//
//     buf2.Append("!");
//     EXPECT_NE(buf1, buf2);
// }
//
// TEST(IoBufferComparison, EqualityWrapped)
// {
//     IoBuffer buf1(64);
//     IoBuffer buf2(256);  // Not wrapped
//
//     // Make buf1 wrapped
//     buf1.Append(std::string(50, 'X'));
//     buf1.Consume(45);
//     buf1.Append("Test");
//
//     // buf2 has same logical content
//     buf2.Append("XXXXXTest");
//
//     EXPECT_EQ(buf1, buf2);
// }
//
// TEST(IoBufferComparison, EqualityEmpty)
// {
//     IoBuffer buf1(64);
//     IoBuffer buf2(256);
//
//     EXPECT_EQ(buf1, buf2);  // Both empty
// }
//
// // =============================================================================
// // Edge Cases
// // =============================================================================
//
// TEST(IoBufferEdgeCases, EmptyOperations)
// {
//     IoBuffer buf(64);
//
//     EXPECT_TRUE(buf.ReadableSpan().empty());
//
//     auto [r1, r2] = buf.ReadableSpans();
//     EXPECT_TRUE(r1.empty());
//     EXPECT_TRUE(r2.empty());
//
//     EXPECT_FALSE(buf.Find('x').has_value());
//     EXPECT_FALSE(buf.FindCRLF().has_value());
// }
//
// TEST(IoBufferEdgeCases, FullBuffer)
// {
//     IoBuffer buf(64);
//
//     // Fill completely
//     buf.Append(std::string(64, 'F'));
//
//     EXPECT_TRUE(buf.IsFull());
//     EXPECT_EQ(buf.WritableBytes(), 0u);
//     EXPECT_TRUE(buf.WritableSpan().empty());
//
//     // Should still be readable
//     EXPECT_EQ(buf.ReadableBytes(), 64u);
// }
//
// TEST(IoBufferEdgeCases, Reset)
// {
//     IoBuffer buf(64);
//     buf.Append("Some Data");
//
//     buf.Reset();
//
//     EXPECT_TRUE(buf.IsEmpty());
//     EXPECT_EQ(buf.ReadableBytes(), 0u);
//     EXPECT_EQ(buf.Capacity(), 64u);  // Capacity preserved
// }
//
// TEST(IoBufferEdgeCases, Clear)
// {
//     IoBuffer buf(64);
//     buf.Append("Data");
//
//     buf.Clear();  // Alias for Reset
//
//     EXPECT_TRUE(buf.IsEmpty());
// }
//
// TEST(IoBufferEdgeCases, ConsumeThrowsOnOverflow)
// {
//     IoBuffer buf(64);
//     buf.Append("Short");
//
//     EXPECT_THROW(buf.Consume(100), std::out_of_range);
// }
//
// TEST(IoBufferEdgeCases, CommitThrowsOnOverflow)
// {
//     IoBuffer buf(64);
//
//     EXPECT_THROW(buf.Commit(100), std::out_of_range);
// }
//
// TEST(IoBufferEdgeCases, AppendNullCString)
// {
//     IoBuffer buf(64);
//
//     buf.Append(static_cast<const char*>(nullptr));
//
//     EXPECT_TRUE(buf.IsEmpty());  // No crash, nothing appended
// }
//
// TEST(IoBufferEdgeCases, AppendEmptyData)
// {
//     IoBuffer buf(64);
//     buf.Append("Existing");
//
//     buf.Append("");
//     buf.Append(std::string_view{});
//     buf.Append(std::span<const char>{});
//
//     EXPECT_EQ(buf.ToString(), "Existing");  // Unchanged
// }
//
// // =============================================================================
// // ToVector Tests
// // =============================================================================
//
// TEST(IoBufferConversion, ToVector)
// {
//     IoBuffer buf(256);
//     buf.Append("VectorTest");
//
//     auto vec = buf.ToVector();
//
//     EXPECT_EQ(vec.size(), 10u);
//     EXPECT_EQ(std::string(vec.data(), vec.size()), "VectorTest");
// }
//
// TEST(IoBufferConversion, ToVectorWrapped)
// {
//     IoBuffer buf(64);
//
//     // Create wrapped buffer
//     buf.Append(std::string(50, 'W'));
//     buf.Consume(45);
//     buf.Append("rap");
//
//     auto vec = buf.ToVector();
//
//     EXPECT_EQ(vec.size(), buf.ReadableBytes());
//     EXPECT_EQ(std::string(vec.data(), vec.size()), "WWWWWrap");
// }
//
// // =============================================================================
// // Skip Tests
// // =============================================================================
//
// TEST(IoBufferSkip, SkipIsAliasForConsume)
// {
//     IoBuffer buf(256);
//     buf.Append("SkipThis");
//
//     buf.Skip(4);
//
//     EXPECT_EQ(buf.ToString(), "This");
// }
//
// // =============================================================================
// // Compact Tests (No-op for ring buffer)
// // =============================================================================
//
// TEST(IoBufferCompact, CompactIsNoOp)
// {
//     IoBuffer buf(64);
//     buf.Append("Data");
//     buf.Consume(2);
//
//     size_t readable_before = buf.ReadableBytes();
//     std::string content_before = buf.ToString();
//
//     // Re-add the content for the test
//     buf.Reset();
//     buf.Append(content_before);
//
//     buf.Compact();  // Should do nothing
//
//     EXPECT_EQ(buf.ReadableBytes(), readable_before);
// }

// =============================================================================
// Stress Tests
// =============================================================================

TEST(IoBufferStress, ManySmallWritesAndReads)
{
    IoBuffer buf(64);

    for (int i = 0; i < 1000; ++i)
    {
        buf.Append("X");
        if (buf.ReadableBytes() > 32)
        {
            buf.Consume(16);
        }
    }

    // Should not crash, data should be consistent
    EXPECT_LE(buf.ReadableBytes(), buf.Capacity());
}

// TEST(IoBufferStress, AlternatingLargeOperations)
// {
//     IoBuffer buf(256);
//
//     for (int i = 0; i < 100; ++i)
//     {
//         std::string data(128, 'A' + (i % 26));
//         buf.Append(data);
//
//         if (buf.ReadableBytes() > 256)
//         {
//             buf.Consume(128);
//         }
//     }
//
//     EXPECT_FALSE(buf.IsEmpty());
// }

// =============================================================================
// Main (provided by gtest_main, but explicit for clarity)
// =============================================================================

// If not linking with gtest_main:
// int main(int argc, char** argv) {
//     ::testing::InitGoogleTest(&argc, argv);
//     return RUN_ALL_TESTS();
// }