#include "uring/logger.hpp"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

TEST(LoggerTest, BasicLogging)
{
    using namespace kio::ALOG;
    set_level(level::debug);

    KIO_LOG_DEBUG("This is a debug message: {}", 42);
    KIO_LOG_INFO("This is an info message: {}", "hello");
    KIO_LOG_WARN("This is a warning message");
    KIO_LOG_ERROR("This is an error message: {:x}", 0xDEADBEEF);

    // Give some time for the worker thread to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(LoggerTest, LevelFiltering)
{
    using namespace kio::ALOG;
    set_level(level::warn);

    // These should not appear (manually verified if output is visible)
    KIO_LOG_DEBUG("SHOULD NOT SEE THIS (DEBUG)");
    KIO_LOG_INFO("SHOULD NOT SEE THIS (INFO)");

    // This should appear
    KIO_LOG_WARN("SHOULD SEE THIS (WARN)");
    KIO_LOG_ERROR("SHOULD SEE THIS (ERROR)");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(LoggerTest, RapidLogging)
{
    using namespace kio::ALOG;
    set_level(level::info);

    for (int i = 0; i < 100; ++i)
    {
        KIO_LOG_INFO("Message number {}", i);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

namespace
{
int g_format_calls = 0;
struct CountsFormatting
{
};
}  // namespace

template <>
struct std::formatter<CountsFormatting> : std::formatter<int>
{
    auto format(const CountsFormatting&, std::format_context& ctx) const
    {
        ++g_format_calls;
        return std::formatter<int>::format(0, ctx);
    }
};

// A message below the runtime level must not be formatted at all.
TEST(LoggerTest, FilteredMessagesAreNotFormatted)
{
    using namespace kio::ALOG;
    set_level(level::warn);
    g_format_calls = 0;

    KIO_LOG_DEBUG("never {}", CountsFormatting{});
    KIO_LOG_INFO("never {}", CountsFormatting{});
    EXPECT_EQ(g_format_calls, 0);

    KIO_LOG_WARN("once {}", CountsFormatting{});
    EXPECT_EQ(g_format_calls, 1);

    set_level(level::info);
}
