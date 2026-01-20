#include <fcntl.h>
#include <unistd.h>

#include "io_pool/runtime.hpp"
#include "io_pool/io.hpp"
#include <gtest/gtest.h>
#include <cstdio>

using namespace uring::io;
using namespace uring;

class ExecutorTest : public ::testing::Test {
protected:
    Runtime rt;
    ExecutorTest() : rt(RuntimeConfig{.num_threads = 1}) {
        rt.loop_forever(); // Start the background thread
    }
    ~ExecutorTest() { rt.stop(); }
};

TEST_F(ExecutorTest, PipeReadWrite)
{
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    
    // Set non-blocking (good practice for uring)
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    const std::string payload = "Hello uring";
    std::vector<char> recv_buf(payload.size());

    auto task = [&](ThreadContext& ctx) -> Task<void> {
        // Write
        auto res_w = co_await io::write(ctx, fds[1], std::span(payload));
        EXPECT_TRUE(res_w.has_value());
        EXPECT_EQ(*res_w, payload.size());

        // Read
        auto res_r = co_await read(ctx, fds[0], std::span(recv_buf));
        EXPECT_TRUE(res_r.has_value());
        EXPECT_EQ(*res_r, payload.size());
    };

    // Run on thread 0 and wait
    block_on(rt.thread(0), task(rt.thread(0)));

    EXPECT_EQ(std::string(recv_buf.data(), recv_buf.size()), payload);
    
    close(fds[0]);
    close(fds[1]);
}

// test/test_runtime.cpp
TEST_F(ExecutorTest, MultiThreadScheduling) {
    std::atomic counter{0};
    constexpr int num_tasks = 1000;
    std::latch done_latch{num_tasks};

    auto task = [&](ThreadContext& ctx) -> Task<> {
        counter.fetch_add(1, std::memory_order_relaxed);
        done_latch.count_down();
        co_return;
    };

    // Schedule tasks round-robin
    for (int i = 0; i < num_tasks; i++) {
        rt.schedule(task(rt.next_thread()));
    }

    done_latch.wait();

    EXPECT_EQ(counter.load(), num_tasks);
}


TEST_F(ExecutorTest, ManySmallReads) {

    auto test_task = [&](ThreadContext& ctx) -> Task<> {
        std::string temp_file = std::tmpnam(nullptr);

        const auto fd_res = co_await io::openat(ctx, temp_file,
            O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(fd_res.has_value());
        const int fd = *fd_res;

        // Write 1MB
        std::vector data(1024 * 1024, 'A');
        co_await write_exact(ctx, fd, data);

        // Read in 1KB chunks
        char buf[1024];
        size_t total = 0;
        for (int i = 0; i < 1024; i++) {
            auto res = co_await read_exact_at(ctx, fd,
                std::span{buf, 1024}, i * 1024);
            EXPECT_TRUE(res.has_value());
            total += 1024;
        }

        EXPECT_EQ(total, data.size());

        co_await io::close(ctx, fd);
        std::remove(temp_file.c_str());
    };

    block_on(rt.thread(0), test_task(rt.thread(0)));
}