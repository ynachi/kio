//
// Created by Yao ACHI on 04/01/2026.
//

#include "kio/resp/writer.h"

#include "kio/core/chunked_buffer.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace kio::io;
using namespace kio::resp;

class RespWriterTest : public ::testing::Test
{
protected:
    ChunkedBuffer buffer_;
    RespWriter writer{buffer_};

    // Helper to get the full committed content as a string
    [[nodiscard]] std::string GetContent() const
    {
        std::string s;
        auto iovs = buffer_.GetIoVecs();
        for (const auto& iov : iovs)
        {
            s.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
        }
        return s;
    }
};

TEST_F(RespWriterTest, WriteSimpleString)
{
    writer.WriteSimpleString("OK");
    writer.Commit();
    EXPECT_EQ(GetContent(), "+OK\r\n");
}

TEST_F(RespWriterTest, WriteError)
{
    writer.WriteError("ERR unknown command");
    writer.Commit();
    EXPECT_EQ(GetContent(), "-ERR unknown command\r\n");
}

TEST_F(RespWriterTest, WriteInteger)
{
    writer.WriteInteger(1000);
    writer.WriteInteger(-42);
    writer.Commit();
    EXPECT_EQ(GetContent(), ":1000\r\n:-42\r\n");
}

TEST_F(RespWriterTest, WriteBulkString)
{
    writer.WriteBulkString("hello");
    writer.Commit();
    EXPECT_EQ(GetContent(), "$5\r\nhello\r\n");
}

TEST_F(RespWriterTest, WriteEmptyBulkString)
{
    writer.WriteBulkString("");
    writer.Commit();
    EXPECT_EQ(GetContent(), "$0\r\n\r\n");
}

TEST_F(RespWriterTest, WriteNullBulkString)
{
    writer.WriteNullBulk();
    writer.Commit();
    EXPECT_EQ(GetContent(), "$-1\r\n");
}

TEST_F(RespWriterTest, WriteArray)
{
    // Array of 2 elements: ["foo", "bar"]
    writer.WriteArrayHeader(2);
    writer.WriteBulkString("foo");
    writer.WriteBulkString("bar");
    writer.Commit();

    EXPECT_EQ(GetContent(), "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
}

TEST_F(RespWriterTest, WriteNullArray)
{
    writer.WriteNullArray();
    writer.Commit();
    EXPECT_EQ(GetContent(), "*-1\r\n");
}

TEST_F(RespWriterTest, WriteNestedArray)
{
    // [1, [2, 3]]
    writer.WriteArrayHeader(2);
    writer.WriteInteger(1);

    writer.WriteArrayHeader(2);
    writer.WriteInteger(2);
    writer.WriteInteger(3);
    writer.Commit();

    EXPECT_EQ(GetContent(), "*2\r\n:1\r\n*2\r\n:2\r\n:3\r\n");
}

TEST_F(RespWriterTest, RollbackDiscardsUncommitted)
{
    writer.WriteSimpleString("KeepMe");
    writer.Commit();

    writer.WriteSimpleString("DiscardMe");
    // Do NOT commit
    writer.Rollback();

    EXPECT_EQ(GetContent(), "+KeepMe\r\n");
}