// tests/aio/io_helpers_tests.cpp
// Tests for exact read/write helpers and sendfile

#include <array>
#include <chrono>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/io_helpers.hpp"
#include "test_helpers.hpp"
#include <gtest/gtest.h>

using namespace aio;
using namespace aio::test;
using namespace std::chrono_literals;

class IoHelpersTest : public ::testing::Test {
protected:
    IoContext ctx{256};
};

// -----------------------------------------------------------------------------
// AsyncReadExact Tests
// -----------------------------------------------------------------------------

TEST_F(IoHelpersTest, ReadExactSuccess) {
    auto file = MakeTempFileWithContent("hello world!");
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 5> buf{};
        auto rr = co_await AsyncReadExact(ctx, file.Get(), buf, 0);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(AsString(buf), "hello");
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoHelpersTest, ReadExactMultipleChunks) {
    // Create a larger file that may require multiple reads
    std::string content(8192, 'X');
    auto file = MakeTempFileWithContent(content);
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<> {
        std::vector<std::byte> buf(8192);
        auto rr = co_await AsyncReadExact(ctx, file.Get(), buf, 0);
        EXPECT_TRUE(rr.has_value());

        // Verify content
        for (size_t i = 0; i < buf.size(); ++i) {
            EXPECT_EQ(buf[i], std::byte{'X'}) << "Mismatch at index " << i;
        }
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoHelpersTest, ReadExactEOF) {
    auto file = MakeTempFileWithContent("short");
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 100> buf{};  // Larger than file
        auto rr = co_await AsyncReadExact(ctx, file.Get(), buf, 0);
        EXPECT_FALSE(rr.has_value());  // Should fail - EOF before filled
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// AsyncWriteExact Tests
// -----------------------------------------------------------------------------

TEST_F(IoHelpersTest, WriteExactSuccess) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        auto data = GenerateTestData(1024);
        auto wr = co_await AsyncWriteExact(ctx, file.Get(), data, 0);
        EXPECT_TRUE(wr.has_value());

        // Read back and verify
        std::vector<std::byte> read_buf(1024);
        ::lseek(file.Get(), 0, SEEK_SET);
        auto bytes_read = ::read(file.Get(), read_buf.data(), read_buf.size());
        EXPECT_EQ(bytes_read, 1024);
        EXPECT_TRUE(SpansEqual(data, read_buf));
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoHelpersTest, WriteExactLargeBuffer) {
    auto file = MakeTempFile();
    ASSERT_TRUE(file.Valid());

    auto test = [&]() -> Task<void> {
        // Write 64KB - may require multiple writes
        auto data = GenerateTestData(65536);
        auto wr = co_await AsyncWriteExact(ctx, file.Get(), data, 0);
        EXPECT_TRUE(wr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    EXPECT_EQ(GetFileSize(file.Get()), 65536);
}

// -----------------------------------------------------------------------------
// AsyncRecvExact Tests
// -----------------------------------------------------------------------------

TEST_F(IoHelpersTest, RecvExactSuccess) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    // Send in one shot
    std::string msg = "exactly this";
    [[maybe_unused]] auto w = ::write(sockets.fd1.Get(), msg.data(), msg.size());

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 12> buf{};  // "exactly this" = 12 chars
        auto rr = co_await AsyncRecvExact(ctx, sockets.fd2.Get(), buf);
        EXPECT_TRUE(rr.has_value());
        EXPECT_EQ(AsString(buf), "exactly this");
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

TEST_F(IoHelpersTest, RecvExactConnectionClosed) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    // Send partial data then close
    [[maybe_unused]] auto w = ::write(sockets.fd1.Get(), "partial", 7);
    sockets.fd1.Close();

    auto test = [&]() -> Task<void> {
        std::array<std::byte, 100> buf{};  // Larger than what's available
        auto rr = co_await AsyncRecvExact(ctx, sockets.fd2.Get(), buf);
        EXPECT_FALSE(rr.has_value());  // Should fail
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);
}

// -----------------------------------------------------------------------------
// AsyncSendExact Tests
// -----------------------------------------------------------------------------

TEST_F(IoHelpersTest, SendExactSuccess) {
    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        auto data = GenerateTestData(256);
        auto sr = co_await AsyncSendExact(ctx, sockets.fd1.Get(), data);
        EXPECT_TRUE(sr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    // Verify by reading
    std::array<std::byte, 256> buf{};
    auto bytes_read = ::read(sockets.fd2.Get(), buf.data(), buf.size());
    EXPECT_EQ(bytes_read, 256);
    EXPECT_TRUE(VerifyTestData(buf));
}

// -----------------------------------------------------------------------------
// AsyncSendfile Tests
// -----------------------------------------------------------------------------

TEST_F(IoHelpersTest, SendfileSmallFile) {
    // Create source file with known content
    auto src = MakeTempFileWithSize(1024, std::byte{0xAB});
    ASSERT_TRUE(src.Valid());

    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        auto sr = co_await AsyncSendfile(ctx, sockets.fd1.Get(), src.Get(), 0, 1024);
        EXPECT_TRUE(sr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    // Read from receiver and verify
    std::array<std::byte, 1024> buf{};
    size_t total = 0;
    while (total < 1024) {
        auto n = ::read(sockets.fd2.Get(), buf.data() + total, buf.size() - total);
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
    EXPECT_EQ(total, 1024u);

    // Verify content
    for (size_t i = 0; i < buf.size(); ++i) {
        EXPECT_EQ(buf[i], std::byte{0xAB}) << "Mismatch at index " << i;
    }
}

TEST_F(IoHelpersTest, SendfileLargeFile) {
    // Test with file larger than chunk size (64KB)
    constexpr size_t file_size = 256 * 1024;  // 256KB
    auto src = MakeTempFileWithSize(file_size, std::byte{0xCD});
    ASSERT_TRUE(src.Valid());

    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        auto sr = co_await AsyncSendfile(ctx, sockets.fd1.Get(), src.Get(), 0, file_size);
        EXPECT_TRUE(sr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    // Verify size by reading
    size_t total = 0;
    std::array<std::byte, 4096> buf{};
    while (total < file_size) {
        auto n = ::read(sockets.fd2.Get(), buf.data(), buf.size());
        if (n <= 0) break;
        total += static_cast<size_t>(n);

        // Verify chunk content
        for (ssize_t i = 0; i < n; ++i) {
            EXPECT_EQ(buf[static_cast<size_t>(i)], std::byte{0xCD});
        }
    }
    EXPECT_EQ(total, file_size);
}

TEST_F(IoHelpersTest, SendfileWithOffset) {
    // File: "AAABBBCCC" - we'll send just "BBB"
    auto src = MakeTempFileWithContent("AAABBBCCC");
    ASSERT_TRUE(src.Valid());

    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        // Send 3 bytes starting at offset 3
        auto sr = co_await AsyncSendfile(ctx, sockets.fd1.Get(), src.Get(), 3, 3);
        EXPECT_TRUE(sr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    std::array<std::byte, 3> buf{};
    auto n = ::read(sockets.fd2.Get(), buf.data(), buf.size());
    EXPECT_EQ(n, 3);
    EXPECT_EQ(AsString(buf), "BBB");
}

TEST_F(IoHelpersTest, SendfilePartialFile) {
    // Create 1KB file, send only first 512 bytes
    auto src = MakeTempFileWithSize(1024, std::byte{0xEF});
    ASSERT_TRUE(src.Valid());

    auto sockets = MakeSocketPair();
    ASSERT_TRUE(sockets.Valid());

    auto test = [&]() -> Task<void> {
        auto sr = co_await AsyncSendfile(ctx, sockets.fd1.Get(), src.Get(), 0, 512);
        EXPECT_TRUE(sr.has_value());
        co_return;
    };

    auto task = test();
    ctx.RunUntilDone(task);

    std::array<std::byte, 512> buf{};
    auto n = ::read(sockets.fd2.Get(), buf.data(), buf.size());
    EXPECT_EQ(n, 512);
}

// -----------------------------------------------------------------------------
// PipePool Tests (via sendfile reuse)
// -----------------------------------------------------------------------------

TEST_F(IoHelpersTest, SendfileReusesPipes) {
    // Multiple sendfile operations should reuse pipes from the pool
    auto src = MakeTempFileWithSize(512, std::byte{0x11});
    ASSERT_TRUE(src.Valid());

    auto sockets1 = MakeSocketPair();
    auto sockets2 = MakeSocketPair();
    auto sockets3 = MakeSocketPair();
    ASSERT_TRUE(sockets1.Valid() && sockets2.Valid() && sockets3.Valid());

    auto test = [&]() -> Task<void> {
        // Three sequential sendfile operations
        auto r1 = co_await AsyncSendfile(ctx, sockets1.fd1.Get(), src.Get(), 0, 512);
        EXPECT_TRUE(r1.has_value());

        ::lseek(src.Get(), 0, SEEK_SET);
        auto r2 = co_await AsyncSendfile(ctx, sockets2.fd1.Get(), src.Get(), 0, 512);
        EXPECT_TRUE(r2.has_value());

        ::lseek(src.Get(), 0, SEEK_SET);
        auto r3 = co_await AsyncSendfile(ctx, sockets3.fd1.Get(), src.Get(), 0, 512);
        EXPECT_TRUE(r3.has_value());

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