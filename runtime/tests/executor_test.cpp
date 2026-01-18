#include <gtest/gtest.h>
#include "runtime/runtime.hpp"
#include <unistd.h>
#include <fcntl.h>

using namespace uring;

class ExecutorTest : public ::testing::Test {
protected:
    Runtime rt;
    ExecutorTest() : rt(RuntimeConfig{.num_threads = 1}) {
        rt.loop_forever(); // Start the background thread
    }
    ~ExecutorTest() { rt.stop(); }
};

TEST_F(ExecutorTest, PipeReadWrite) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    
    // Set non-blocking (good practice for uring)
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    const std::string payload = "Hello uring";
    std::vector<char> recv_buf(payload.size());

    auto task = [&](ThreadContext& ctx) -> Task<void> {
        // Write
        auto res_w = co_await write(ctx.executor(), fds[1], std::span(payload));
        EXPECT_TRUE(res_w.has_value());
        EXPECT_EQ(*res_w, payload.size());

        // Read
        auto res_r = co_await read(ctx.executor(), fds[0], std::span(recv_buf));
        EXPECT_TRUE(res_r.has_value());
        EXPECT_EQ(*res_r, payload.size());
    };

    // Run on thread 0 and wait
    block_on(rt.thread(0), task(rt.thread(0)));

    EXPECT_EQ(std::string(recv_buf.data(), recv_buf.size()), payload);
    
    close(fds[0]);
    close(fds[1]);
}