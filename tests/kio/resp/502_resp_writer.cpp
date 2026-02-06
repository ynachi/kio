//
// Updated for kio::resp::RespWriter and kio::IoBuffer
//

#include "kio/aio.hpp"
#include "kio/core/io_helpers.hpp"

#include <span>
#include <string>
#include <vector>

#include <unistd.h>

#include <sys/socket.h>

#include <gtest/gtest.h>

using namespace kio;
using namespace kio::resp;

class RespWriterTest : public ::testing::Test
{
protected:
    IoBuffer buffer_;
    RespWriter writer_{buffer_};

    [[nodiscard]] std::string RoundTrip()
    {
        kio::IoContext ctx(128);

        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        {
            ADD_FAILURE() << "socketpair failed";
            return {};
        }

        std::error_code err;
        std::string out;

        auto task = [&]() -> Task<void>
        {
            auto span = buffer_.ReadableSpan();
            if (span.empty())
                co_return;

            auto send_res = co_await AsyncSendExact(ctx, fds[0], span);
            if (!send_res)
            {
                err = send_res.error();
                co_return;
            }

            out.resize(span.size());
            auto recv_res = co_await AsyncRecvExact(ctx, fds[1], std::as_writable_bytes(std::span(out)));
            if (!recv_res)
            {
                err = recv_res.error();
                co_return;
            }
        };

        ctx.RunUntilDone(task());
        ctx.CancelAllPending();
        ::close(fds[0]);
        ::close(fds[1]);

        if (err)
        {
            ADD_FAILURE() << "async io failed: " << err.message();
            return {};
        }

        return out;
    }

    void SetUp() override
    {
        // Logging setup if needed
    }
};

TEST_F(RespWriterTest, WriteSimpleString)
{
    writer_.WriteSimpleString("OK");
    writer_.Commit();
    EXPECT_EQ(RoundTrip(), "+OK\r\n");
}

TEST_F(RespWriterTest, WriteError)
{
    writer_.WriteError("ERR unknown command");
    writer_.Commit();
    EXPECT_EQ(RoundTrip(), "-ERR unknown command\r\n");
}

TEST_F(RespWriterTest, WriteInteger)
{
    writer_.WriteInteger(1000);
    writer_.WriteInteger(-42);
    writer_.Commit();
    EXPECT_EQ(RoundTrip(), ":1000\r\n:-42\r\n");
}

TEST_F(RespWriterTest, WriteBulkString)
{
    writer_.WriteBulkString("hello");
    writer_.Commit();
    EXPECT_EQ(RoundTrip(), "$5\r\nhello\r\n");
}

TEST_F(RespWriterTest, WriteEmptyBulkString)
{
    writer_.WriteBulkString("");
    writer_.Commit();
    EXPECT_EQ(RoundTrip(), "$0\r\n\r\n");
}

TEST_F(RespWriterTest, WriteNullBulkString)
{
    writer_.WriteNullBulk();
    writer_.Commit();
    EXPECT_EQ(RoundTrip(), "$-1\r\n");
}

TEST_F(RespWriterTest, WriteArray)
{
    // Array of 2 elements: ["foo", "bar"]
    writer_.WriteArrayHeader(2);
    writer_.WriteBulkString("foo");
    writer_.WriteBulkString("bar");
    writer_.Commit();

    EXPECT_EQ(RoundTrip(), "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
}

TEST_F(RespWriterTest, WriteNullArray)
{
    writer_.WriteNullArray();
    writer_.Commit();
    EXPECT_EQ(RoundTrip(), "*-1\r\n");
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

    EXPECT_EQ(RoundTrip(), "*2\r\n:1\r\n*2\r\n:2\r\n:3\r\n");
}

TEST_F(RespWriterTest, RollbackDiscardsUncommitted)
{
    writer_.WriteSimpleString("KeepMe");
    writer_.Commit();

    writer_.WriteSimpleString("DiscardMe");
    // Do NOT commit
    writer_.Rollback();

    EXPECT_EQ(RoundTrip(), "+KeepMe\r\n");
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
