#include "kio/kio.hpp"

#include <gtest/gtest.h>

#include "test_helpers.hpp"

using namespace kio;
using namespace kio::test;

namespace
{
struct ThrowingSubmitOp : kio::DispatchOp<ThrowingSubmitOp>
{
    explicit ThrowingSubmitOp(kio::IoContext& ctx) : DispatchOp(&ctx) {}

    using DispatchOp<ThrowingSubmitOp>::await_resume;
};

inline void Submit(kio::UringBackend&, kio::IoContext&, ThrowingSubmitOp&)
{
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "synthetic submit failure");
}

inline void SubmitWithTimeout(kio::UringBackend&, kio::IoContext&, ThrowingSubmitOp&, __kernel_timespec&)
{
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                            "synthetic submit failure");
}
}  // namespace

TEST(IoContextTest, Lifecycle) {
    IoContext ctx;
    Task<> t = []() -> Task<> { co_return; }();
    ctx.RunUntilDone(std::move(t));
}

TEST(IoContextTest, ReturnValue) {
    IoContext ctx;
    Task<int> t = []() -> Task<int> { co_return 42; }();
    ctx.RunUntilDone(std::move(t));
    ASSERT_EQ(t.Result(), 42);
}

TEST(IoContextTest, Notify) {
    IoContext ctx;
    bool notified = false;
    
    // Start a thread that waits a bit then notifies
    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        notified = ctx.Notify();
    });

    // Run a task that just sleeps (or waits for something)
    // Here we just use RunUntilDone with a simple task to ensure loop runs
    Task<> task = [&](IoContext& c) -> Task<> {
        co_await AsyncSleep(c, std::chrono::milliseconds(50));
    }(ctx);
    
    ctx.RunUntilDone(std::move(task));
    t.join();
    
    ASSERT_TRUE(notified);
}

TEST(IoContextTest, SubmitFailureReturnsError) {
    IoContext ctx;
    auto task = [&](IoContext& c) -> Task<Result<size_t>> {
        co_return co_await ThrowingSubmitOp(c);
    }(ctx);

    ctx.RunUntilDone(std::move(task));
    auto result = task.Result();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), EAGAIN);
    EXPECT_EQ(result.error().category(), std::system_category());
}

TEST(IoContextTest, TimeoutSubmitFailureReturnsError) {
    IoContext ctx;
    auto task = [&](IoContext& c) -> Task<Result<size_t>> {
        co_return co_await ThrowingSubmitOp(c).WithTimeout(std::chrono::milliseconds(1));
    }(ctx);

    ctx.RunUntilDone(std::move(task));
    auto result = task.Result();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), EAGAIN);
    EXPECT_EQ(result.error().category(), std::system_category());
}

TEST(IoContextTest, IoErrorMatchingIgnoresDetail) {
    const auto base = make_error_code(EBADF);
    const auto detailed = base.WithDetail(IoErrorDetail{.op = IoOperation::Read, .fd = 7, .offset = 4096, .len = 512});

    EXPECT_EQ(detailed, base);
    EXPECT_EQ(detailed.kind(), IoErrorKind::BadFd);
    EXPECT_EQ(detailed, std::errc::bad_file_descriptor);
}

TEST(IoContextTest, IoErrorPreservesParseKind) {
    const IoError err(ParseError::Incomplete);

    EXPECT_EQ(err, ParseError::Incomplete);
    EXPECT_EQ(err.kind(), IoErrorKind::Incomplete);
}

TEST(IoContextTest, ResizeGrowAndShrink) {
    IoContext ctx(8);

    if (!ctx.SupportsRingResize()) {
        GTEST_SKIP() << "ring resize requires Linux >= 6.13 with liburing resize support";
    }

    const auto initial_sq = ctx.SqEntries();
    const auto initial_cq = ctx.CqEntries();

    ASSERT_TRUE(ctx.Resize(initial_sq * 2).has_value());
    EXPECT_GE(ctx.SqEntries(), initial_sq * 2);
    EXPECT_GE(ctx.CqEntries(), ctx.SqEntries());

    auto task = [&](IoContext& c) -> Task<Result<void>> {
        co_return co_await AsyncSleep(c, std::chrono::milliseconds(1));
    }(ctx);
    ctx.RunUntilDone(std::move(task));
    ASSERT_TRUE(task.Result().has_value());

    ASSERT_TRUE(ctx.Resize(initial_sq, initial_cq).has_value());
    EXPECT_EQ(ctx.SqEntries(), initial_sq);
    EXPECT_EQ(ctx.CqEntries(), initial_cq);
}

TEST(IoContextTest, ResizeRejectsInvalidSizes) {
    IoContext ctx(8);

    auto zero_sq = ctx.Resize(0);
    ASSERT_FALSE(zero_sq.has_value());
    EXPECT_EQ(zero_sq.error().kind(), IoErrorKind::Invalid);

    auto zero_cq = ctx.Resize(8, 0);
    ASSERT_FALSE(zero_cq.has_value());
    EXPECT_EQ(zero_cq.error().kind(), IoErrorKind::Invalid);

    auto too_small_cq = ctx.Resize(8, 4);
    ASSERT_FALSE(too_small_cq.has_value());
    EXPECT_EQ(too_small_cq.error().kind(), IoErrorKind::Invalid);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
