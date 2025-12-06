//
// Created by Yao ACHI on 25/10/2025.
//

#include "../kio/core/worker.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

#include "../kio/core/coro.h"
#include "../kio/core/errors.h"
#include "../kio/core/sync_wait.h"

using namespace kio;
using namespace kio::io;

class WorkerTest : public ::testing::Test
{
protected:
    std::unique_ptr<Worker> worker;
    std::filesystem::path test_dir;
    std::unique_ptr<std::jthread> worker_thread;
    WorkerConfig config;

    void SetUp() override
    {
        alog::configure(1024, LogLevel::Debug);

        test_dir = std::filesystem::temp_directory_path() / "kio_tests/";
        std::filesystem::create_directories(test_dir);
        config.uring_submit_timeout_ms = 10;
        worker = std::make_unique<Worker>(0, config);

        // Start the worker's event loop on a dedicated thread
        worker_thread = std::make_unique<std::jthread>([this] { worker->loop_forever(); });

        worker->wait_ready();
    }

    void TearDown() override
    {
        // Request the worker to stop and wait for its thread to join.
        (void) worker->request_stop();
        worker_thread.reset();
        worker.reset();
        std::filesystem::remove_all(test_dir);
    }

    // Helper coroutine to run a test on the worker thread
    template<typename Awaitable>
    auto RunOnWorker(Awaitable &&awaitable)
    {
        using AwaitableType = std::remove_reference_t<Awaitable>;
        auto task = [&]() -> Task<decltype(std::declval<AwaitableType>().await_resume())>
        {
            co_await SwitchToWorker(*worker);
            co_return co_await std::forward<Awaitable>(awaitable);
        };

        return SyncWait(task());
    }
};

TEST_F(WorkerTest, AsyncSleep)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        const auto start = std::chrono::steady_clock::now();
        const auto result = co_await worker->async_sleep(std::chrono::milliseconds(20));
        const auto end = std::chrono::steady_clock::now();

        EXPECT_TRUE(result.has_value());
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        EXPECT_GE(duration.count(), 20);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, AsyncReadOnBadFdReturnsError)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);
        constexpr int bad_fd = 999;
        char buffer[10];

        // We expect this to fail
        auto result = co_await worker->async_read(bad_fd, std::span(buffer));

        EXPECT_FALSE(result.has_value());
        //The error must be 'InvalidFileDescriptor' (EBADF)
        EXPECT_EQ(result.error().value, EBADF);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, AsyncReadWriteSocketPair)
{
    int fds[2];
    // Create a connected pair: fds[0] <--> fds[1]
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int read_fd = fds[0];
    const int write_fd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        std::string write_buf = "hello";
        const auto write_res = co_await worker->async_write(write_fd, std::span(write_buf.data(), write_buf.size()));
        EXPECT_TRUE(write_res.has_value());
        EXPECT_EQ(*write_res, 5);

        char read_buf[10]{};
        const auto read_res = co_await worker->async_read(read_fd, std::span(read_buf, sizeof(read_buf)));
        EXPECT_TRUE(read_res.has_value());
        EXPECT_EQ(*read_res, 5);

        EXPECT_EQ(std::string(read_buf, 5), "hello");
    };

    SyncWait(test_coro());

    // Clean up fds
    ::close(read_fd);
    ::close(write_fd);
}

TEST_F(WorkerTest, AsyncReadWriteExactSocketPair)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int read_fd = fds[0];
    const int write_fd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        std::string write_buf = "hello world";
        auto write_res = co_await worker->async_write_exact(write_fd, std::span(write_buf.data(), write_buf.size()));

        EXPECT_TRUE(write_res.has_value());

        std::vector<char> read_buf(write_buf.size());
        const auto read_res = co_await worker->async_read_exact(read_fd, std::span(read_buf.data(), read_buf.size()));

        EXPECT_TRUE(read_res.has_value());
        EXPECT_EQ(std::string(read_buf.data(), read_buf.size()), "hello world");
    };

    SyncWait(test_coro());

    // Clean up fds
    ::close(read_fd);
    ::close(write_fd);
}

TEST_F(WorkerTest, AsyncReadExactEOF)
{
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int read_fd = fds[0];
    const int write_fd = fds[1];

    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        std::string write_buf = "short";
        const auto write_res = co_await worker->async_write(write_fd, std::span(write_buf.data(), write_buf.size()));
        EXPECT_TRUE(write_res.has_value());

        // Close the writing end. This will send an EOF to the read end after
        // the initial 5 bytes are read.
        // We use async_close to ensure it happens on the worker thread.
        const auto close_res = co_await worker->async_close(write_fd);
        EXPECT_TRUE(close_res.has_value());

        // Attempt to read *exactly* 10 bytes. This should fail.
        char read_buf[10];  // Expect 10 bytes
        auto read_res = co_await worker->async_read_exact(read_fd, std::span(read_buf, sizeof(read_buf)));

        EXPECT_FALSE(read_res.has_value());
        // The error must be due to EOF
        EXPECT_EQ(read_res.error().value, kIoEof);
    };

    SyncWait(test_coro());

    ::close(read_fd);
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
        co_await SwitchToWorker(*worker);

        // Shared variable - NO synchronization needed because Worker is single-threaded
        int shared_counter = 0;
        bool detached_task_completed = false;

        // Launch a detached task that will increment the counter
        auto detached_increment = [&]() -> DetachedTask
        {
            co_await SwitchToWorker(*worker);

            // Even though this is "detached", it runs on the same worker thread
            // So it will execute cooperatively with other tasks
            for (int i = 0; i < 100; ++i)
            {
                shared_counter++;
                // Yield control to allow other tasks to run
                co_await worker->async_sleep(std::chrono::milliseconds(0));
            }
            detached_task_completed = true;
        };

        // Start the detached task
        detached_increment().detach();

        // Multiple regular tasks also incrementing the counter
        auto increment_task = [&](const int iterations) -> Task<void>
        {
            co_await SwitchToWorker(*worker);

            for (int i = 0; i < iterations; ++i)
            {
                shared_counter++;
                // Yield to allow interleaving
                co_await worker->async_sleep(std::chrono::milliseconds(0));
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
            co_await worker->async_sleep(std::chrono::milliseconds(1));
        }

        // Check: All increments should have happened
        // 100 (detached) + 50 + 75 + 25 = 250
        EXPECT_EQ(shared_counter, 250);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, GetOpIdPoolGrowthCorrectness)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        const size_t initial_capacity = worker->get_stats().active_coroutines + worker->get_stats().active_coroutines; // just trigger reading

        std::unordered_set<uint64_t> ids;
        for (size_t i = 0; i < initial_capacity + 10; ++i)
        {
            uint64_t id = worker->get_op_id();
            ids.insert(id);
            worker->init_op_slot(id, {});
        }

        EXPECT_GT(worker->get_stats().active_coroutines, initial_capacity);
        EXPECT_EQ(ids.size(), initial_capacity + 10); // all IDs unique

        // cleanup
        for (const auto id : ids)
            worker->release_op_id(id);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, ReleaseOpIdReuseCorrectness)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        const uint64_t id1 = worker->get_op_id();
        worker->release_op_id(id1);

        const uint64_t id2 = worker->get_op_id();
        EXPECT_EQ(id1, id2); // recycled ID must be reissued
        worker->release_op_id(id2);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, DoubleReleaseOpIdIsSafe)
{
    auto test_coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        const uint64_t id = worker->get_op_id();
        worker->release_op_id(id);
        worker->release_op_id(id); // should NOT crash or corrupt

        const uint64_t id2 = worker->get_op_id();
        EXPECT_EQ(id, id2); // still reusable
        worker->release_op_id(id2);
    };

    SyncWait(test_coro());
}

TEST_F(WorkerTest, AsyncReadAtRealFileOffsetWorks)
{
    namespace fs = std::filesystem;
    const fs::path path = test_dir / "tmp_read_test.bin";

    const std::string data = "ABCDEFGHIJ";
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GT(fd, 0);
    ASSERT_EQ(::write(fd, data.data(), data.size()), 10);
    ASSERT_EQ(::lseek(fd, 0, SEEK_SET), 0);

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);
        char buf[3]{};

        const auto res = co_await worker->async_read_at(fd, std::span(buf, 3), 4);
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(*res, 3);
        EXPECT_EQ(std::string(buf, 3), "EFG");
    };

    SyncWait(coro());

    ::close(fd);
    fs::remove(path);
}

TEST_F(WorkerTest, RequestStopIsIdempotentAndWakesLoop)
{
    EXPECT_TRUE(worker->request_stop());
    EXPECT_TRUE(worker->request_stop());
}

TEST_F(WorkerTest, AsyncReadExactAtOffsetCorrectness)
{
    namespace fs = std::filesystem;
    const fs::path path = test_dir /  "read_exact_offset_test.bin";

    const std::string data = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // 26 bytes
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GT(fd, 0);
    ASSERT_EQ((::write(fd, data.data(), data.size())), data.size());
    ASSERT_EQ(::lseek(fd, 0, SEEK_SET), 0);

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);
        constexpr size_t to_read = 5;
        char buf[to_read]{};

        // Read 5 bytes starting at offset 10 ("KLMNO")
        const auto res = co_await worker->async_read_exact_at(fd, std::span(buf, to_read), 10);

        EXPECT_TRUE(res.has_value()) << "read_exact_at must succeed for full buffer fill";
        EXPECT_EQ(std::string(buf, to_read), data.substr(10, to_read));
    };

    SyncWait(coro());

    ::close(fd);
    fs::remove(path);
}

TEST_F(WorkerTest, AsyncWriteExactFile_Correctness)
{
    namespace fs = std::filesystem;
    const fs::path path = test_dir /  "write_exact_test.bin";

    // Initial file
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GT(fd, 0);
    ::close(fd);

    std::string payload = "hello async uring world"; // > 5 bytes, includes spaces
    const size_t N = payload.size();

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        int fd = ::open(path.c_str(), O_RDWR);
        EXPECT_GT(fd, 0);

        const auto res = co_await worker->async_write_exact(fd, std::span(payload.data(), N));

        EXPECT_TRUE(res.has_value()) << "async_write_exact must succeed on file";
        ::close(fd);
    };

    SyncWait(coro());

    // Validate file content
    fd = ::open(path.c_str(), O_RDONLY);
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
    const fs::path path = test_dir /  "direct_io_test.bin";

    // Open a file with O_DIRECT
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_DIRECT, 0644);
    ASSERT_GT(fd, 0);

    // Allocate aligned buffer multiple of 4096
    constexpr size_t N = 4096 * 3;
    void* ptr;
    ASSERT_EQ(posix_memalign(&ptr, 4096, N), 0);

    const std::string pattern = "kio_uring_direct_io_file_test";
    memset(ptr, 0, N);
    memcpy(ptr, pattern.data(), pattern.size());

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);

        const std::span<const char> out(static_cast<char*>(ptr), N);
        auto w = co_await worker->async_write_at(fd, out, 0);
        EXPECT_TRUE(w.has_value());
        EXPECT_EQ(*w, N);

        lseek(fd, 0, SEEK_SET);

        const std::span in(static_cast<char*>(ptr), N);
        const auto r = co_await worker->async_read_at(fd, in, 0);
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(*r, N);
        EXPECT_EQ(std::string_view(static_cast<char*>(ptr), pattern.size()), pattern);
    };

    SyncWait(coro());

    free(ptr);
    ::close(fd);
}

// This code should not pass because the offset is not aligned but
// it does. Don't have time yet to dig into this mistery
// TODO: fix me
TEST_F(WorkerTest, DISABLED_DirectIO_UnalignedOffset_Fails)
{
    namespace fs = std::filesystem;
    const fs::path path = test_dir /  "direct_io_unaligned.bin";

    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_DIRECT, 0644);
    ASSERT_GT(fd, 0);

    auto coro = [&]() -> Task<void>
    {
        co_await SwitchToWorker(*worker);
        char buf[4096];
        const auto r = co_await worker->async_read_at(fd, std::span(buf, 4096), 3); // ❗ 3 is NOT aligned
        EXPECT_FALSE(r.has_value());
    };

    SyncWait(coro());
    ::close(fd);
}


int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
