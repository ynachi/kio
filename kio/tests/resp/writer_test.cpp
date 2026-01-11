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
    RespWriter writer_{buffer_};

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

    void SetUp() override { kio::alog::Configure(1024, kio::LogLevel::kDisabled); }
};

TEST_F(RespWriterTest, WriteSimpleString)
{
    writer_.WriteSimpleString("OK");
    writer_.Commit();
    EXPECT_EQ(GetContent(), "+OK\r\n");
}

TEST_F(RespWriterTest, WriteError)
{
    writer_.WriteError("ERR unknown command");
    writer_.Commit();
    EXPECT_EQ(GetContent(), "-ERR unknown command\r\n");
}

TEST_F(RespWriterTest, WriteInteger)
{
    writer_.WriteInteger(1000);
    writer_.WriteInteger(-42);
    writer_.Commit();
    EXPECT_EQ(GetContent(), ":1000\r\n:-42\r\n");
}

TEST_F(RespWriterTest, WriteBulkString)
{
    writer_.WriteBulkString("hello");
    writer_.Commit();
    EXPECT_EQ(GetContent(), "$5\r\nhello\r\n");
}

TEST_F(RespWriterTest, WriteEmptyBulkString)
{
    writer_.WriteBulkString("");
    writer_.Commit();
    EXPECT_EQ(GetContent(), "$0\r\n\r\n");
}

TEST_F(RespWriterTest, WriteNullBulkString)
{
    writer_.WriteNullBulk();
    writer_.Commit();
    EXPECT_EQ(GetContent(), "$-1\r\n");
}

TEST_F(RespWriterTest, WriteArray)
{
    // Array of 2 elements: ["foo", "bar"]
    writer_.WriteArrayHeader(2);
    writer_.WriteBulkString("foo");
    writer_.WriteBulkString("bar");
    writer_.Commit();

    EXPECT_EQ(GetContent(), "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
}

TEST_F(RespWriterTest, WriteNullArray)
{
    writer_.WriteNullArray();
    writer_.Commit();
    EXPECT_EQ(GetContent(), "*-1\r\n");
}

TEST_F(RespWriterTest, WriteNestedArray)
{
    // [1, [2, 3]]
    writer_.WriteArrayHeader(2);
    writer_.WriteInteger(1);

    writer_.WriteArrayHeader(2);
    writer_.WriteInteger(2);
    writer_.WriteInteger(3);
    writer_.Commit();

    EXPECT_EQ(GetContent(), "*2\r\n:1\r\n*2\r\n:2\r\n:3\r\n");
}

TEST_F(RespWriterTest, RollbackDiscardsUncommitted)
{
    writer_.WriteSimpleString("KeepMe");
    writer_.Commit();

    writer_.WriteSimpleString("DiscardMe");
    // Do NOT commit
    writer_.Rollback();

    EXPECT_EQ(GetContent(), "+KeepMe\r\n");
}