//
// Created by Yao ACHI on 25/10/2025.
//

#include "../kio/fs/fs.h"
#include "kio/sync/sync_wait.h"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

using namespace kio;
using namespace kio::io;

class FileManagerTest : public ::testing::Test
{
protected:
    const char* test_filename = "affinity_test_file.txt";
    WorkerConfig config;
    std::unique_ptr<FileManager> fm;

    void SetUp() override
    {
        // Create a dummy file
        std::ofstream outfile(test_filename);
        outfile << "delete me";
        outfile.close();

        // Create a FileManager with multiple workers
        config.uring_submit_timeout_ms = 10;
        fm = std::make_unique<FileManager>(4, config);

        // Wait for all workers in the pool to be ready
        for (size_t i = 0; i < fm->Pool().NumWorkers(); ++i)
        {
            fm->Pool().GetWorker(i)->WaitReady();
        }
    }

    void TearDown() override
    {
        // FileManager destructor will stop the pool
        fm.reset();
        std::filesystem::remove(test_filename);
    }
};

// This test verifies that two 'File' objects opened from the same path
// are processed by the same worker thread.
TEST_F(FileManagerTest, FileAffinity)
{
    auto get_thread_id = [&](File& file) -> Task<std::thread::id>
    {
        // This 'async_read' will run on the file's assigned worker thread.
        // We use it as a way to execute code on that thread.
        char buf[1];
        (void)co_await file.AsyncRead(std::span(buf), 0);
        // Return the ID of the thread we are currently on
        co_return std::this_thread::get_id();
    };

    auto test_coro = [&]() -> Task<void>
    {
        // Open the *same file path* twice
        auto file1_exp = co_await fm->AsyncOpen(test_filename, O_RDONLY, 0);
        auto file2_exp = co_await fm->AsyncOpen(test_filename, O_RDONLY, 0);

        EXPECT_TRUE(file1_exp.has_value());
        EXPECT_TRUE(file2_exp.has_value());

        auto& file1 = file1_exp.value();
        auto& file2 = file2_exp.value();

        // Get the thread ID used to process each file handle
        const std::thread::id id1 = co_await get_thread_id(file1);
        const std::thread::id id2 = co_await get_thread_id(file2);

        // The thread IDs MUST be the same, proving affinity
        EXPECT_EQ(id1, id2);
    };

    SyncWait(test_coro());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
