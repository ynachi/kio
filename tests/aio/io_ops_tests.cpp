// tests/aio/io_ops_tests.cpp
// Integration tests for I/O operations

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/uio.h>
#include <unistd.h>

#include "aio/io_context.hpp"
#include "aio/io.hpp"
#include "aio/io_helpers.hpp"
#include "test_helpers.hpp"

using namespace aio;
using namespace aio::test;
using namespace std::chrono_literals;

class IoOpsTest : public ::testing::Test {
protected:
    IoContext ctx{256};
};

// -----------------------------------------------------------------------------
// AsyncRead / AsyncWrite Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, WriteAndReadFile) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<> {
        std::array write_data{
            std::byte{'h'}, std::byte{'e'}, std::byte{'l'},
            std::byte{'l'}, std::byte{'o'}
        };

        auto wr = co_await AsyncWrite(ctx, file.Get(), write_data, 0);
        EXPECT_TRUE(wr.has_value());
        EXPECT_EQ(*wr, 5u);

        std::array<std::byte, 5> read_data{};
        auto rr = co_await AsyncRead(ctx, file.Get(), read_data, 0);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(*rr, 5u);

        EXPECT_EQ(std::memcmp(write_data.data(), read_data.data(), 5), 0);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoOpsTest, ReadAtOffset) {
    auto file = MakeTempFileWithContent("0123456789");
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<> {
        std::array<std::byte, 3> buf{};
        auto rr = co_await AsyncRead(ctx, file.Get(), buf, 5);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(*rr, 3u);
        EXPECT_EQ(AsString(buf), "567");
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoOpsTest, WriteAtOffset) {
    auto file = MakeTempFileWithContent("----------");
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        auto data = AsBytes("XXX");
        auto wr = co_await AsyncWrite(ctx, file.Get(), data, 3);
        EXPECT_TRUE(wr.has_value());
        EXPECT_EQ(*wr, 3u);

        // Read back entire file
        std::array<std::byte, 10> buf{};
        auto rr = co_await AsyncRead(ctx, file.Get(), buf, 0);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(AsString(buf), "---XXX----");
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoOpsTest, ReadEOF) {
    auto file = MakeTempFileWithContent("short");
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 100> buf{};
        auto rr = co_await AsyncRead(ctx, file.Get(), buf, 0);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(*rr, 5u);  // Only 5 bytes available
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// AsyncRecv / AsyncSend Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, SendRecvSocketPair) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        auto sr = co_await AsyncSend(ctx, sockets.fd1.Get(), "hello");
        EXPECT_TRUE(sr.has_value());
        EXPECT_EQ(*sr, 5u);

        std::array<std::byte, 16> buf{};
        auto rr = co_await AsyncRecv(ctx, sockets.fd2.Get(), buf);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(*rr, 5u);
        EXPECT_EQ(AsString(std::span{buf}.subspan(0, 5)), "hello");
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoOpsTest, SendStringView) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<> {
        std::string_view msg = "test message";
        auto sr = co_await AsyncSend(ctx, sockets.fd1.Get(), msg);
        EXPECT_TRUE(sr.has_value());
        EXPECT_EQ(*sr, msg.size());

        std::array<std::byte, 32> buf{};
        auto rr = co_await AsyncRecv(ctx, sockets.fd2.Get(), buf);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(AsString(std::span{buf}.subspan(0, *rr)), "test message");
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoOpsTest, RecvConnectionClosed) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    // Close sender side
    sockets.fd1.Close();

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 16> buf{};
        auto rr = co_await AsyncRecv(ctx, sockets.fd2.Get(), buf);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(*rr, 0u);  // 0 = connection closed
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// AsyncReadv / AsyncWritev Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, WritevReadv) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        // Scatter-gather write
        std::array<std::byte, 4> buf1{std::byte{'A'}, std::byte{'B'}, std::byte{'C'}, std::byte{'D'}};
        std::array<std::byte, 4> buf2{std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}};

        std::array<iovec, 2> write_iov{{
            {buf1.data(), buf1.size()},
            {buf2.data(), buf2.size()}
        }};

        auto wr = co_await AsyncWritev(ctx, file.Get(), write_iov, 0);
        EXPECT_TRUE(wr.has_value());
        EXPECT_EQ(*wr, 8u);

        // Scatter-gather read into different buffers
        std::array<std::byte, 2> read1{};
        std::array<std::byte, 6> read2{};

        std::array<iovec, 2> read_iov{{
            {read1.data(), read1.size()},
            {read2.data(), read2.size()}
        }};

        auto rr = co_await AsyncReadv(ctx, file.Get(), read_iov, 0);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(*rr, 8u);

        EXPECT_EQ(AsString(read1), "AB");
        EXPECT_EQ(AsString(read2), "CD1234");
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// AsyncFsync / AsyncFdatasync Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, FsyncAfterWrite) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        auto data = AsBytes("durable data");
        auto wr = co_await AsyncWrite(ctx, file.Get(), data, 0);
        EXPECT_TRUE(wr.has_value());

        auto fs = co_await AsyncFsync(ctx, file.Get());
        EXPECT_TRUE(fs.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoOpsTest, FdatasyncAfterWrite) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<> {
        auto data = AsBytes("durable data");
        auto wr = co_await AsyncWrite(ctx, file.Get(), data, 0);
        EXPECT_TRUE(wr.has_value());

        auto fs = co_await AsyncFdatasync(ctx, file.Get());
        EXPECT_TRUE(fs.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// AsyncFtruncate Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, FtruncateExtend) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        auto tr = co_await AsyncFtruncate(ctx, file.Get(), 1024);
        EXPECT_TRUE(tr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    EXPECT_EQ(GetFileSize(file.Get()), 1024);
}

TEST_F(IoOpsTest, FtruncateShrink) {
    auto file = MakeTempFileWithContent("0123456789");
    ASSERT_TRUE(file.Valid());
    EXPECT_EQ(GetFileSize(file.Get()), 10);

    auto test = [&]() -> Task<void> {
        auto tr = co_await AsyncFtruncate(ctx, file.Get(), 5);
        EXPECT_TRUE(tr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    EXPECT_EQ(GetFileSize(file.Get()), 5);
}

// -----------------------------------------------------------------------------
// AsyncFallocate Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, FallocatePreallocate) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        auto fa = co_await AsyncFallocate(ctx, file.Get(), 0, 0, 4096);
        EXPECT_TRUE(fa.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    EXPECT_GE(GetFileSize(file.Get()), 4096);
}

// -----------------------------------------------------------------------------
// AsyncPoll Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, PollReadable) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    // Write to fd1, poll fd2 for readability
    [[maybe_unused]] auto w = ::write(sockets.fd1.Get(), "x", 1);

    auto test = [&]() -> Task<void> {
        auto pr = co_await AsyncPoll(ctx, sockets.fd2.Get(), POLLIN);
        EXPECT_TRUE(pr.has_value());
        EXPECT_TRUE(*pr & POLLIN);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// AsyncSleep Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, SleepTiming) {
    auto test = [&]() -> Task<void> {
        auto start = std::chrono::steady_clock::now();
        co_await AsyncSleep(ctx, 50ms);
        auto elapsed = std::chrono::steady_clock::now() - start;

        // Allow some tolerance
        EXPECT_GE(elapsed, 40ms);
        EXPECT_LE(elapsed, 100ms);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoOpsTest, SleepZeroDuration) {
    auto test = [&]() -> Task<void> {
        auto start = std::chrono::steady_clock::now();
        co_await AsyncSleep(ctx, 0ms);
        auto elapsed = std::chrono::steady_clock::now() - start;

        // Should complete very quickly
        EXPECT_LE(elapsed, 50ms);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// AsyncClose Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, CloseFile) {
    int raw_fd;
    {
        auto file = MakeTempFile();
        ASSERT_TRUE(file.Valid());
        raw_fd = file.Release();
    }

    auto test = [&]() -> Task<void> {
        auto cr = co_await AsyncClose(ctx, raw_fd);
        EXPECT_TRUE(cr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    // Verify fd is closed (write should fail)
    EXPECT_EQ(::write(raw_fd, "x", 1), -1);
    EXPECT_EQ(errno, EBADF);
}

// -----------------------------------------------------------------------------
// Error Handling Tests
// -----------------------------------------------------------------------------

TEST_F(IoOpsTest, ReadInvalidFd) {
    auto test = [&]() -> Task<void> {
        std::array<std::byte, 16> buf{};
        auto rr = co_await AsyncRead(ctx, -1, buf, 0);
        EXPECT_FALSE(rr.has_value());
        EXPECT_EQ(rr.error().value(), EBADF);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoOpsTest, WriteInvalidFd) {
    auto test = [&]() -> Task<void> {
        auto data = AsBytes("test");
        auto wr = co_await AsyncWrite(ctx, -1, data, 0);
        EXPECT_FALSE(wr.has_value());
        EXPECT_EQ(wr.error().value(), EBADF);
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
