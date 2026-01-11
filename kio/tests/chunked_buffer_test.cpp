//
// Created by Your Name on 03/01/2026.
//

#include "kio/core/chunked_buffer.h"

#include "kio/core/worker.h"
#include "kio/sync/sync_wait.h"

#include <unistd.h>

#include <sys/socket.h>

#include <gtest/gtest.h>

using namespace kio;
using namespace kio::io;

class ChunkedBufferTest : public ::testing::Test
{
protected:
    static constexpr size_t kSmallChunkSize = 16;
    ChunkedBuffer buffer_{kSmallChunkSize};
};

TEST_F(ChunkedBufferTest, AppendAndCommit)
{
    constexpr std::string_view kData1 = "Hello";
    constexpr std::string_view kData2 = "World";

    buffer_.Append(kData1);
    EXPECT_EQ(buffer_.CommittedSize(), 0);
    EXPECT_FALSE(buffer_.HasData());

    buffer_.Commit();
    EXPECT_EQ(buffer_.CommittedSize(), 5);
    EXPECT_TRUE(buffer_.HasData());

    buffer_.Append(kData2);
    EXPECT_EQ(buffer_.CommittedSize(), 5);

    buffer_.Commit();
    EXPECT_EQ(buffer_.CommittedSize(), 10);
}

TEST_F(ChunkedBufferTest, RollbackUncommitted)
{
    buffer_.Append(std::string_view("KeepMe"));
    buffer_.Commit();

    buffer_.Append(std::string_view("DiscardMe"));
    EXPECT_EQ(buffer_.CommittedSize(), 6);

    buffer_.Rollback();

    EXPECT_EQ(buffer_.CommittedSize(), 6);
    auto iovs = buffer_.GetIoVecs();
    ASSERT_EQ(iovs.size(), 1);
    EXPECT_EQ(std::string((char*)iovs[0].iov_base, iovs[0].iov_len), "KeepMe");
}

TEST_F(ChunkedBufferTest, MultiChunkAllocation)
{
    std::string const kLargeData(40, 'A');

    buffer_.Append(std::string_view(kLargeData));
    buffer_.Commit();

    EXPECT_EQ(buffer_.CommittedSize(), 40);

    auto iovs = buffer_.GetIoVecs();
    // 40 bytes / 16 byte chunks = 16 + 16 + 8
    ASSERT_EQ(iovs.size(), 3);

    EXPECT_EQ(iovs[0].iov_len, 16);
    EXPECT_EQ(iovs[1].iov_len, 16);
    EXPECT_EQ(iovs[2].iov_len, 8);

    std::string reconstructed;
    for (const auto& iov : iovs)
    {
        reconstructed.append(static_cast<char*>(iov.iov_base), iov.iov_len);
    }
    EXPECT_EQ(reconstructed, kLargeData);
}

TEST_F(ChunkedBufferTest, PartialConsumption)
{
    const std::string kData = "1234567890";  // 10 bytes
    buffer_.Append(std::string_view(kData));
    buffer_.Commit();
    buffer_.Consume(4);

    EXPECT_EQ(buffer_.CommittedSize(), 6);

    auto iovs = buffer_.GetIoVecs();
    ASSERT_EQ(iovs.size(), 1);
    EXPECT_EQ(std::string((char*)iovs[0].iov_base, 6), "567890");
}

class ChunkedBufferUsageTest : public ::testing::Test
{
protected:
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<std::jthread> worker_thread_;
    WorkerConfig config_;

    void SetUp() override
    {
        config_.uring_submit_timeout_ms = 1;
        worker_ = std::make_unique<Worker>(0, config_);
        worker_thread_ = std::make_unique<std::jthread>([this] { worker_->LoopForever(); });
        worker_->WaitReady();
    }

    void TearDown() override
    {
        (void)worker_->RequestStop();
        worker_thread_.reset();
        worker_.reset();
    }
};

TEST_F(ChunkedBufferUsageTest, FramedWriteExample)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int kReadFd = fds[0];
    const int kWriteFd = fds[1];

    auto server_logic = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        ChunkedBuffer out_buf;

        // 1. Prepare Data
        std::string const kPayload = R"({"status": "ok", "data": "very important content"})";
        uint32_t length = htonl(static_cast<uint32_t>(kPayload.size()));

        // 2. Append Header + Body
        out_buf.Append(std::span<const char>(reinterpret_cast<char*>(&length), sizeof(length)));
        out_buf.Append(std::string_view(kPayload));

        // 3. Commit
        out_buf.Commit();

        // 4. Send using Scatter-Gather
        while (out_buf.HasData())
        {
            auto iovs = out_buf.GetIoVecs();

            auto res = co_await worker_->AsyncWritev(kWriteFd, iovs.data(), static_cast<int>(iovs.size()),
                                                     static_cast<uint64_t>(-1));

            if (!res)
            {
                break;
            }

            out_buf.Consume(*res);
        }

        (void)co_await worker_->AsyncClose(kWriteFd);
    };

    auto client_logic = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);
        uint32_t net_len = 0;
        char header_buf[4];
        auto read_res = co_await worker_->AsyncReadExact(kReadFd, std::span(header_buf, 4));
        EXPECT_TRUE(read_res.has_value());

        memcpy(&net_len, header_buf, 4);
        uint32_t const kLen = ntohl(net_len);

        std::vector<char> body_buf(kLen);
        read_res = co_await worker_->AsyncReadExact(kReadFd, std::span(body_buf));
        EXPECT_TRUE(read_res.has_value());

        std::string const kReceived(body_buf.begin(), body_buf.end());
        EXPECT_EQ(kReceived, R"({"status": "ok", "data": "very important content"})");

        (void)co_await worker_->AsyncClose(kReadFd);
    };

    auto t1 = server_logic();
    auto t2 = client_logic();
    SyncWait(std::move(t1));
    SyncWait(std::move(t2));
}