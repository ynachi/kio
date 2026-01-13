//
// Created by Your Name on 03/01/2026.
//

#include "kio/core/worker.h"
#include "kio/resp/parser.h"
#include "kio/resp/writer.h"
#include "kio/sync/sync_wait.h"

#include <string>

#include <unistd.h>

#include <sys/socket.h>

#include <gtest/gtest.h>

using namespace kio;
using namespace kio::io;
using namespace kio::resp;

class WriteDecodeTest : public ::testing::Test
{
protected:
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<std::jthread> worker_thread_;
    WorkerConfig config_;

    ChunkedBuffer write_buffer_;
    RespWriter writer_{write_buffer_};
    Parser parser_{ParserConfig{}};

    int server_fd_{-1};
    int client_fd_{-1};

    void SetUp() override
    {
        alog::Configure(1024, LogLevel::kDisabled);
        config_.uring_submit_timeout_ms = 1;
        worker_ = std::make_unique<Worker>(0, config_);
        worker_thread_ = std::make_unique<std::jthread>([this] { worker_->LoopForever(); });
        worker_->WaitReady();

        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        server_fd_ = fds[0];
        client_fd_ = fds[1];
    }

    void TearDown() override
    {
        if (server_fd_ >= 0)
        {
            ::close(server_fd_);
        }
        if (client_fd_ >= 0)
        {
            ::close(client_fd_);
        }
        (void)worker_->RequestStop();
        worker_thread_.reset();
        worker_.reset();
    }

    // Helper: Run the full IO loop
    Task<Result<void>> FlushAndRead()
    {
        co_await SwitchToWorker(*worker_);

        KIO_TRY(co_await writer_.Flush(*worker_, server_fd_));

        // B. Read from Kernel to Parser (AsyncRead)
        // We loop until we have read something or run out of data
        // For tests, we assume one "Flush" results in readable data.
        while (worker_->IsRunning())
        {
            auto writable = parser_.Buffer().WritableSpan();
            auto res = KIO_TRY(co_await worker_->AsyncRead(client_fd_, writable));

            if (res == 0)
            {
                break;  // EOF
            }

            parser_.Buffer().CommitWrite(res);

            break;
        }

        co_return {};
    }

    static void Run(Task<void>&& task) { SyncWait(std::move(task)); }
};

TEST_F(WriteDecodeTest, RoundTripSimpleTypesIO)
{
    auto test_logic = [&]() -> Task<void>
    {
        // 1. Prepare Data
        writer_.WriteSimpleString("OK");
        writer_.WriteInteger(12345);
        writer_.WriteError("oops");
        writer_.Commit();  // Make visible for Flush

        // 2. Perform IO
        co_await FlushAndRead();

        // 3. Verify Parser
        // Frame 1: Simple String
        const auto kRes1 = parser_.NextFrame();
        EXPECT_TRUE(kRes1.has_value());
        EXPECT_EQ(kRes1->type, FrameType::kSimpleString);
        EXPECT_EQ(GetSimplePayload(*kRes1), "OK");
        parser_.Consume(*kRes1);

        // Frame 2: Integer
        const auto kRes2 = parser_.NextFrame();
        EXPECT_TRUE(kRes2.has_value());
        EXPECT_EQ(kRes2->type, FrameType::kInteger);
        EXPECT_EQ(GetSimplePayload(*kRes2), "12345");
        parser_.Consume(*kRes2);

        // Frame 3: Error
        const auto kRes3 = parser_.NextFrame();
        EXPECT_TRUE(kRes3.has_value());
        EXPECT_EQ(kRes3->type, FrameType::kSimpleError);
        EXPECT_EQ(GetSimplePayload(*kRes3), "oops");
        parser_.Consume(*kRes3);
    };

    Run(test_logic());
}

TEST_F(WriteDecodeTest, RoundTripBulkStringIO)
{
    auto test_logic = [&]() -> Task<void>
    {
        std::string const kPayload = "This is a bulk string payload";
        writer_.WriteBulkString(kPayload);
        writer_.Commit();

        co_await FlushAndRead();

        const auto kRes = parser_.NextFrame();
        EXPECT_TRUE(kRes.has_value());
        EXPECT_EQ(kRes->type, FrameType::kBulkString);
        EXPECT_EQ(GetBulkPayload(*kRes), kPayload);
    };
    Run(test_logic());
}

TEST_F(WriteDecodeTest, RoundTripComplexArrayIO)
{
    auto test_logic = [&]() -> Task<void>
    {
        // Array: [ "user:100", 100, [ "role", "admin" ] ]
        writer_.WriteArrayHeader(3);
        writer_.WriteBulkString("user:100");
        writer_.WriteInteger(100);

        writer_.WriteArrayHeader(2);
        writer_.WriteBulkString("role");
        writer_.WriteBulkString("admin");
        writer_.Commit();

        co_await FlushAndRead();

        auto res = parser_.NextFrame();
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(res->type, FrameType::kArray);
        EXPECT_EQ(res->element_count, 3);

        FrameIterator iter(*res, parser_);

        auto el1 = iter.Next();
        EXPECT_TRUE(el1.has_value());
        EXPECT_EQ(GetBulkPayload(*el1), "user:100");

        auto el2 = iter.Next();
        EXPECT_TRUE(el2.has_value());
        EXPECT_EQ(GetSimplePayload(*el2), "100");

        auto el3 = iter.Next();
        EXPECT_TRUE(el3.has_value());
        EXPECT_EQ(el3->type, FrameType::kArray);
        EXPECT_EQ(el3->element_count, 2);

        FrameIterator sub_iter(*el3, parser_);
        auto sub1 = sub_iter.Next();
        EXPECT_EQ(GetBulkPayload(*sub1), "role");
        auto sub2 = sub_iter.Next();
        EXPECT_EQ(GetBulkPayload(*sub2), "admin");
    };
    Run(test_logic());
}

TEST_F(WriteDecodeTest, RoundTripNullsIO)
{
    auto test_logic = [&]() -> Task<void>
    {
        writer_.WriteArrayHeader(2);
        writer_.WriteNullBulk();
        writer_.WriteNullArray();
        writer_.Commit();

        co_await FlushAndRead();

        const auto kRes = parser_.NextFrame();
        EXPECT_TRUE(kRes.has_value());

        FrameIterator iter(*kRes, parser_);
        // Null Bulk
        const auto kEl1 = iter.Next();
        EXPECT_TRUE(kEl1.has_value());
        EXPECT_EQ(kEl1->type, FrameType::kBulkString);
        EXPECT_TRUE(GetBulkPayload(*kEl1).empty());

        // Null Array
        const auto kEl2 = iter.Next();
        EXPECT_TRUE(kEl2.has_value());
        EXPECT_EQ(kEl2->type, FrameType::kArray);
        EXPECT_EQ(kEl2->element_count, 0);
    };
    Run(test_logic());
}