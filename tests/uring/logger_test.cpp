#include "uring/logger.hpp"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

TEST(LoggerTest, BasicLogging)
{
    using namespace URing::ALOG;
    set_level(Level::Debug);

    ALOG_DEBUG("This is a debug message: {}", 42);
    ALOG_INFO("This is an info message: {}", "hello");
    ALOG_WARN("This is a warning message");
    ALOG_ERROR("This is an error message: {:x}", 0xDEADBEEF);

    // Give some time for the worker thread to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(LoggerTest, LevelFiltering)
{
    using namespace URing::ALOG;
    set_level(Level::Warn);

    // These should not appear (manually verified if output is visible)
    ALOG_DEBUG("SHOULD NOT SEE THIS (DEBUG)");
    ALOG_INFO("SHOULD NOT SEE THIS (INFO)");

    // This should appear
    ALOG_WARN("SHOULD SEE THIS (WARN)");
    ALOG_ERROR("SHOULD SEE THIS (ERROR)");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(LoggerTest, RapidLogging)
{
    using namespace URing::ALOG;
    set_level(Level::Info);

    for (int i = 0; i < 100; ++i)
    {
        ALOG_INFO("Message number {}", i);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
