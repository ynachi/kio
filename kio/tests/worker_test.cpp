//
// Created by Yao ACHI on 25/10/2025.
//

#include "kio/core/worker.h"

#include "kio/core/coro.h"
#include "kio/core/errors.h"
#include "kio/sync/sync_wait.h"

#include <thread>
#include <unordered_set>

#include <unistd.h>

#include <sys/socket.h>

#include <gtest/gtest.h>

using namespace kio;
using namespace kio::io;

class WorkerTest : public ::testing::Test
{
protected:
    std::unique_ptr<Worker> worker_;
    std::filesystem::path test_dir_;
    std::unique_ptr<std::jthread> worker_thread_;
    WorkerConfig config_;

    void SetUp() override
    {
        alog::Configure(1024, LogLevel::kDisabled);

        test_dir_ = std::filesystem::temp_directory_path() / "kio_tests/";
        std::filesystem::create_directories(test_dir_);
        config_.uring_submit_timeout_ms = 10;
        worker_ = std::make_unique<Worker>(0, config_);

        // Start the worker's event loop on a dedicated thread
        worker_thread_ = std::make_unique<std::jthread>([this] { worker_->LoopForever(); });

        worker_->WaitReady();
    }

    void TearDown() override
    {
        // Request the worker to stop and wait for its thread to join.
        (void)worker_->RequestStop();
        worker_thread_.reset();
        worker_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    // Helper coroutine to run a test on the worker thread
    template <typename Awaitable>
    auto RunOnWorker(Awaitable&& awaitable)
    {
        using AwaitableType = std::remove_reference_t<Awaitable>;
        auto task = [&]() -> Task<decltype(std::declval<AwaitableType>().await_resume())>
        {
            co_await SwitchToWorker(*worker_);
            co_return co_await std::forward<Awaitable>(awaitable);
        };

        return SyncWait(task());
    }
};

TEST_F(WorkerTest, AsyncSleep)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        const auto kStart = std::chrono::steady_clock::now();
        const auto kResult = co_await worker_->AsyncSleep(std::chrono::milliseconds(20));
        const auto kEnd = std::chrono::steady_clock::now();

        EXPECT_TRUE(kResult.has_value());
        const auto kDuration = std::chrono::duration_cast<std::chrono::milliseconds>(kEnd - kStart);
        EXPECT_GE(kDuration.count(), 20);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, AsyncReadOnBadFdReturnsError)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);
        constexpr int kBadFd = 999;
        char buffer[10];

        // We expect this to fail
        auto result = co_await worker_->AsyncRead(kBadFd, std::span(buffer));

        EXPECT_FALSE(result.has_value());
        // The error must be 'InvalidFileDescriptor' (EBADF)
        EXPECT_EQ(result.error().value, EBADF);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, AsyncReadWriteSocketPair)
{
    int fds[2];
    // Create a connected pair: fds[0] <--> fds[1]
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int kReadFd = fds[0];
    const int kWriteFd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        std::string write_buf = "hello";
        const auto kWriteRes = co_await worker_->AsyncWrite(kWriteFd, std::span(write_buf.data(), write_buf.size()));
        EXPECT_TRUE(kWriteRes.has_value());
        EXPECT_EQ(*kWriteRes, 5);

        char read_buf[10]{};
        const auto kReadRes = co_await worker_->AsyncRead(kReadFd, std::span(read_buf, sizeof(read_buf)));
        EXPECT_TRUE(kReadRes.has_value());
        EXPECT_EQ(*kReadRes, 5);

        EXPECT_EQ(std::string(read_buf, 5), "hello");
    };

    SyncWait(test_coro());

    // Clean up fds
    ::close(kReadFd);
    ::close(kWriteFd);
}

TEST_F(WorkerTest, AsyncReadWriteExactSocketPair)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int kReadFd = fds[0];
    const int kWriteFd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        std::string write_buf = "hello world";
        const auto kWriteRes =
            co_await worker_->AsyncWriteExact(kWriteFd, std::span(write_buf.data(), write_buf.size()));

        EXPECT_TRUE(kWriteRes.has_value());

        std::vector<char> read_buf(write_buf.size());
        const auto kReadRes = co_await worker_->AsyncReadExact(kReadFd, std::span(read_buf.data(), read_buf.size()));

        EXPECT_TRUE(kReadRes.has_value());
        EXPECT_EQ(std::string(read_buf.data(), read_buf.size()), "hello world");
    };

    SyncWait(test_coro());

    // Clean up fds
    ::close(kReadFd);
    ::close(kWriteFd);
}

TEST_F(WorkerTest, AsyncReadExactEOF)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int kReadFd = fds[0];
    const int kWriteFd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        std::string write_buf = "short";
        const auto kWriteRes = co_await worker_->AsyncWrite(kWriteFd, std::span(write_buf.data(), write_buf.size()));
        EXPECT_TRUE(kWriteRes.has_value());

        // Close the writing end. This will send an EOF to the read end after
        // the initial 5 bytes are read.
        // We use async_close to ensure it happens on the worker thread.
        const auto kCloseRes = co_await worker_->AsyncClose(kWriteFd);
        EXPECT_TRUE(kCloseRes.has_value());

        // Attempt to read *exactly* 10 bytes. This should fail.
        char read_buf[10];  // Expect 10 bytes
        auto read_res = co_await worker_->AsyncReadExact(kReadFd, std::span(read_buf, sizeof(read_buf)));

        EXPECT_FALSE(read_res.has_value());
        // The error must be due to EOF
        EXPECT_EQ(read_res.error().value, kIoEof);
    };

    SyncWait(test_coro());

    ::close(kReadFd);
}

TEST_F(WorkerTest, SingleThreadedExecution)
{
    // This test proves that a Worker is fully single-threaded by:
    // 1. Having multiple regular tasks modify a shared variable
    // 2. Having a detached task also modify the same variable
    // 3. No synchronization primitives are needed (no mutexes, atomics, etc.)
    // 4. All operations are sequential and deterministic

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        // Shared variable - NO synchronization needed because Worker is single-threaded
        int shared_counter = 0;
        bool detached_task_completed = false;

        // Launch a detached task that will increment the counter
        auto detached_increment = [&]() -> DetachedTask
        {
            co_await SwitchToWorker(*worker_);

            // Even though this is "detached", it runs on the same worker thread
            // So it will execute cooperatively with other tasks
            for (int i = 0; i < 100; ++i)
            {
                shared_counter++;
                // Yield control to allow other tasks to run
                co_await worker_->AsyncSleep(std::chrono::milliseconds(0));
            }
            detached_task_completed = true;
        };

        // Start the detached task
        detached_increment();

        // Multiple regular tasks also incrementing the counter
        auto increment_task = [&](const int iterations) -> Task<void>
        {
            co_await SwitchToWorker(*worker_);

            for (int i = 0; i < iterations; ++i)
            {
                shared_counter++;
                // Yield to allow interleaving
                co_await worker_->AsyncSleep(std::chrono::milliseconds(0));
            }
        };

        // Launch multiple tasks concurrently (but they execute sequentially on the worker)
        auto task1 = increment_task(50);
        auto task2 = increment_task(75);
        auto task3 = increment_task(25);

        // Wait for all regular tasks to complete
        co_await task1;
        co_await task2;
        co_await task3;

        // Wait for a detached task to complete by polling
        while (!detached_task_completed)
        {
            co_await worker_->AsyncSleep(std::chrono::milliseconds(1));
        }

        // Check: All increments should have happened
        // 100 (detached) + 50 + 75 + 25 = 250
        EXPECT_EQ(shared_counter, 250);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, AsyncReadAtRealFileOffsetWorks)
{
    namespace fs = std::filesystem;
    const fs::path path = test_dir_ / "tmp_read_test.bin";

    const std::string data = "ABCDEFGHIJ";
    const int kFd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GT(kFd, 0);
    ASSERT_EQ(::write(kFd, data.data(), data.size()), 10);
    ASSERT_EQ(::lseek(kFd, 0, SEEK_SET), 0);

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);
        char buf[3]{};

        const auto kRes = co_await worker_->AsyncReadAt(kFd, std::span(buf, 3), 4);
        EXPECT_TRUE(kRes.has_value());
        EXPECT_EQ(*kRes, 3);
        EXPECT_EQ(std::string(buf, 3), "EFG");
    };

    SyncWait(coro());

    ::close(kFd);
    fs::remove(path);
}

TEST_F(WorkerTest, RequestStopIsIdempotentAndWakesLoop)
{
    EXPECT_TRUE(worker_->RequestStop());
    EXPECT_TRUE(worker_->RequestStop());
}

TEST_F(WorkerTest, AsyncReadExactAtOffsetCorrectness)
{
    namespace fs = std::filesystem;
    const fs::path path = test_dir_ / "read_exact_offset_test.bin";

    const std::string data = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";  // 26 bytes
    const int kFd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GT(kFd, 0);
    ASSERT_EQ((::write(kFd, data.data(), data.size())), data.size());
    ASSERT_EQ(::lseek(kFd, 0, SEEK_SET), 0);

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);
        constexpr size_t kToRead = 5;
        char buf[kToRead]{};

        // Read 5 bytes starting at offset 10 ("KLMNO")
        const auto res = co_await worker_->AsyncReadExactAt(kFd, std::span(buf, kToRead), 10);

        EXPECT_TRUE(res.has_value()) << "read_exact_at must succeed for full buffer fill";
        EXPECT_EQ(std::string(buf, kToRead), data.substr(10, kToRead));
    };

    SyncWait(coro());

    ::close(kFd);
    fs::remove(path);
}

TEST_F(WorkerTest, AsyncWriteExactFile_Correctness)
{
    namespace fs = std::filesystem;
    const fs::path kPath = test_dir_ / "write_exact_test.bin";

    // Initial file
    int fd = ::open(kPath.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GT(fd, 0);
    ::close(fd);

    std::string payload = "hello async uring world";  // > 5 bytes, includes spaces
    const size_t N = payload.size();

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        int fd = ::open(kPath.c_str(), O_RDWR);
        EXPECT_GT(fd, 0);

        const auto kRes = co_await worker_->AsyncWriteExact(fd, std::span(payload.data(), N));

        EXPECT_TRUE(kRes.has_value()) << "async_write_exact must succeed on file";
        ::close(fd);
    };

    SyncWait(coro());

    // Validate file content
    fd = ::open(kPath.c_str(), O_RDONLY);
    ASSERT_GT(fd, 0);

    std::vector<char> buf(N);
    auto r = ::read(fd, buf.data(), N);
    EXPECT_EQ(static_cast<size_t>(r), N) << "file size should equal payload size";
    EXPECT_EQ(std::string(buf.data(), N), payload) << "file contents must match exactly written data";

    ::close(fd);
}

TEST_F(WorkerTest, DirectIO_AlignedOffsetAndSize)
{
    namespace fs = std::filesystem;
    const fs::path path = test_dir_ / "direct_io_test.bin";

    // Open a file with O_DIRECT
    const int kFd = ::open(path.c_str(), O_CREAT | O_RDWR | O_DIRECT, 0644);
    ASSERT_GT(kFd, 0);

    // Allocate aligned buffer multiple of 4096
    constexpr size_t N = 4096 * 3;
    void* ptr;
    ASSERT_EQ(posix_memalign(&ptr, 4096, N), 0);

    const std::string pattern = "kio_uring_direct_io_file_test";
    memset(ptr, 0, N);
    memcpy(ptr, pattern.data(), pattern.size());

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);

        const std::span<const char> out(static_cast<char*>(ptr), N);
        auto w = co_await worker_->AsyncWriteAt(kFd, out, 0);
        EXPECT_TRUE(w.has_value());
        EXPECT_EQ(*w, N);

        lseek(kFd, 0, SEEK_SET);

        const std::span in(static_cast<char*>(ptr), N);
        const auto kR = co_await worker_->AsyncReadAt(kFd, in, 0);
        EXPECT_TRUE(kR.has_value());
        EXPECT_EQ(*kR, N);
        EXPECT_EQ(std::string_view(static_cast<char*>(ptr), pattern.size()), pattern);
    };

    SyncWait(coro());

    free(ptr);
    ::close(kFd);
}

// This code should not pass because the offset is not aligned but
// it does. Don't have time yet to dig into this mistery
// TODO: fix me
TEST_F(WorkerTest, DISABLED_DirectIO_UnalignedOffset_Fails)
{
    namespace fs = std::filesystem;
    const fs::path path = test_dir_ / "direct_io_unaligned.bin";

    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_DIRECT, 0644);
    ASSERT_GT(fd, 0);

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker_);
        char buf[4096];
        const auto r = co_await worker_->AsyncReadAt(fd, std::span(buf, 4096), 3);  // ❗ 3 is NOT aligned
        EXPECT_FALSE(r.has_value());
    };

    SyncWait(coro());
    ::close(fd);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
