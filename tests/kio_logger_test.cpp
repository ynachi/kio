//
// Created by Yao ACHI on 25/10/2025.
//

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

#include "kio/core/async_logger.h"

using namespace kio;

// Helper to count lines in a string
int count_lines(const std::string &s)
{
    int count = 0;
    for (char c: s)
    {
        if (c == '\n')
        {
            count++;
        }
    }
    return count;
}

TEST(KioLoggerTest, BasicLogging)
{
    std::stringstream ss;
    {
        Logger logger(1024, LogLevel::Info, ss);
        KIO_LOG_INFO(logger, "Hello {}", "World");
    }
    std::string output = ss.str();
    EXPECT_NE(output.find("Hello World"), std::string::npos);
    EXPECT_NE(output.find("[INFO ]"), std::string::npos);
}

TEST(KioLoggerTest, MacroFastPathZeroCost)
{
    std::stringstream ss;
    std::atomic function_called = false;

    {
        auto get_expensive_data = [&]() -> std::string
        {
            function_called.store(true);
            return "This is expensive";
        };
        Logger logger(1024, LogLevel::Info, ss);
        // This log should be compiled out by the macro
        KIO_LOG_DEBUG(logger, "Data: {}", get_expensive_data());
    }

    // The function should *never* have been called
    EXPECT_FALSE(function_called.load());
    EXPECT_TRUE(ss.str().empty());
}

TEST(KioLoggerTest, LogLevelFiltering)
{
    std::stringstream ss;
    {
        Logger logger(1024, LogLevel::Warn, ss);
        KIO_LOG_INFO(logger, "This should not appear");
        KIO_LOG_WARN(logger, "This should appear");
        KIO_LOG_ERROR(logger, "This should also appear");
    }
    std::string output = ss.str();
    EXPECT_EQ(output.find("This should not appear"), std::string::npos);
    EXPECT_NE(output.find("This should appear"), std::string::npos);
    EXPECT_NE(output.find("This should also appear"), std::string::npos);
}

TEST(KioLoggerTest, ChangeLogLevelRuntime)
{
    std::stringstream ss;
    {
        Logger logger(1024, LogLevel::Info, ss);
        KIO_LOG_DEBUG(logger, "First debug");  // Should not appear
        logger.set_level(LogLevel::Debug);
        KIO_LOG_DEBUG(logger, "Second debug");  // Should appear
    }
    std::string output = ss.str();
    EXPECT_EQ(output.find("First debug"), std::string::npos);
    EXPECT_NE(output.find("Second debug"), std::string::npos);
}

TEST(KioLoggerTest, HeapAllocationForLargeMessage)
{
    std::stringstream ss;
    // Create a string larger than the 256-byte stack buffer
    std::string large_string(300, 'A');
    {
        Logger logger(1024, LogLevel::Info, ss);
        KIO_LOG_INFO(logger, "Large: {}", large_string);
    }
    std::string output = ss.str();
    // Check that the *entire* large string was logged correctly
    EXPECT_NE(output.find(large_string), std::string::npos);
}

TEST(KioLoggerTest, MultiThreadedProducerStressTest)
{
    std::stringstream ss;
    constexpr int THREADS = 4;
    constexpr int MSGS_PER_THREAD = 1000;
    {
        Logger logger(8192, LogLevel::Info, ss);
        std::vector<std::thread> producers;
        producers.reserve(THREADS);

        for (int i = 0; i < THREADS; ++i)
        {
            producers.emplace_back(
                    [&logger, i]
                    {
                        for (int j = 0; j < MSGS_PER_THREAD; ++j)
                        {
                            KIO_LOG_INFO(logger, "Message from thread {} num {}", i, j);
                        }
                    });
        }

        for (auto &t: producers)
        {
            t.join();
        }
    }

    std::string output = ss.str();
    // Check that all messages were consumed and written
    EXPECT_EQ(count_lines(output), THREADS * MSGS_PER_THREAD);
}

struct TestSink
{
    std::ostringstream out;
    std::string str() const { return out.str(); }
};

using namespace std::chrono_literals;
TEST(LoggerTests, DisabledLevelProducesNoOutput)
{
    TestSink sink;
    Logger logger(1024, kio::LogLevel::Disabled, sink.out);

    KIO_LOG_INFO(logger, "Should not log");
    KIO_LOG_ERROR(logger, "Nor this one");
    std::this_thread::sleep_for(20ms);

    EXPECT_TRUE(sink.str().empty());
}

TEST(LoggerTests, QueueOverflowDropsMessages)
{
    std::ostringstream oss;
    {
        // Very small queue to force overflow
        Logger logger(4, LogLevel::Info, oss);

        for (int i = 0; i < 100; ++i) KIO_LOG_INFO(logger, "msg {}", i);

        std::this_thread::sleep_for(50ms);

        // Logger destructor will join the consumer thread,
        // ensuring all writes are complete before we read
    }

    // NOW it's safe to read - consumer thread has been joined
    std::string out = oss.str();

    // Not all 100 should appear (some must drop)
    const int count = std::count(out.begin(), out.end(), '\n');
    EXPECT_LT(count, 100);
    EXPECT_GT(count, 0);
}

TEST(LoggerTests, GracefulShutdownFlushesRemaining)
{
    std::ostringstream oss;
    {
        Logger logger(1024, LogLevel::Info, oss);
        for (int i = 0; i < 10; ++i) KIO_LOG_INFO(logger, "shutdown test {}", i);
        // Logger destructor should flush
    }

    auto out = oss.str();
    for (int i = 0; i < 10; ++i)
    {
        EXPECT_NE(out.find(std::to_string(i)), std::string::npos);
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
