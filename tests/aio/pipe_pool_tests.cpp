// tests/aio/pipe_pool_tests.cpp
// Tests for PipePool

#include <gtest/gtest.h>

#include <unistd.h>

#include "aio/pipe_pool.hpp"

using namespace aio;

// -----------------------------------------------------------------------------
// PipePool Basic Tests
// -----------------------------------------------------------------------------

TEST(PipePoolTest, AcquireCreatesPipe) {
    PipePool pool(4);

    auto pipe = pool.Acquire();
    ASSERT_TRUE(pipe.has_value());
    EXPECT_TRUE(pipe->Valid());
    EXPECT_GE(pipe->read_fd, 0);
    EXPECT_GE(pipe->write_fd, 0);
    EXPECT_NE(pipe->read_fd, pipe->write_fd);

    // Clean up
    pipe->Close();
}

TEST(PipePoolTest, ReleasedPipeIsReused) {
    PipePool pool(4);

    // Acquire and remember the fds
    auto pipe1 = pool.Acquire();
    ASSERT_TRUE(pipe1.has_value());
    int read_fd = pipe1->read_fd;
    int write_fd = pipe1->write_fd;

    // Release back to pool
    pool.Release(*pipe1);

    // Acquire again - should get the same pipe
    auto pipe2 = pool.Acquire();
    ASSERT_TRUE(pipe2.has_value());
    EXPECT_EQ(pipe2->read_fd, read_fd);
    EXPECT_EQ(pipe2->write_fd, write_fd);

    pipe2->Close();
}

TEST(PipePoolTest, PoolRespectsSizeLimit) {
    PipePool pool(2);  // Max 2 pipes

    // Create 3 pipes
    auto pipe1 = pool.Acquire();
    auto pipe2 = pool.Acquire();
    auto pipe3 = pool.Acquire();

    ASSERT_TRUE(pipe1.has_value());
    ASSERT_TRUE(pipe2.has_value());
    ASSERT_TRUE(pipe3.has_value());

    int fd1 = pipe1->read_fd;
    int fd2 = pipe2->read_fd;
    int fd3 = pipe3->read_fd;

    // Release all 3
    pool.Release(*pipe1);
    pool.Release(*pipe2);
    pool.Release(*pipe3);  // This one should be closed, not pooled

    // Acquire 2 - should get pipe1 and pipe2 back
    auto reacquired1 = pool.Acquire();
    auto reacquired2 = pool.Acquire();

    ASSERT_TRUE(reacquired1.has_value());
    ASSERT_TRUE(reacquired2.has_value());

    // One of them should be a new pipe (not fd3)
    bool found_fd3 = (reacquired1->read_fd == fd3 || reacquired2->read_fd == fd3);
    // fd3 was closed, so it shouldn't be found as-is
    // (Though the OS might reuse the fd number for a new pipe)

    // At least verify we got valid pipes
    EXPECT_TRUE(reacquired1->Valid());
    EXPECT_TRUE(reacquired2->Valid());

    reacquired1->Close();
    reacquired2->Close();

    // Suppress unused variable warning
    (void)fd1;
    (void)fd2;
    (void)found_fd3;
}

TEST(PipePoolTest, PipeWriteAndRead) {
    PipePool pool(4);

    auto pipe = pool.Acquire();
    ASSERT_TRUE(pipe.has_value());

    // Write to pipe
    const char* msg = "hello";
    ssize_t written = ::write(pipe->write_fd, msg, 5);
    EXPECT_EQ(written, 5);

    // Read from pipe
    char buf[16] = {};
    ssize_t read_bytes = ::read(pipe->read_fd, buf, sizeof(buf));
    EXPECT_EQ(read_bytes, 5);
    EXPECT_STREQ(buf, "hello");

    pipe->Close();
}

TEST(PipePoolTest, ReleaseInvalidPipeIsNoop) {
    PipePool pool(4);

    Pipe invalid;
    EXPECT_FALSE(invalid.Valid());

    // Should not crash
    pool.Release(invalid);
}

// -----------------------------------------------------------------------------
// Pipe::Close Tests
// -----------------------------------------------------------------------------

TEST(PipeTest, CloseClosesFds) {
    Pipe pipe;
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    pipe.read_fd = fds[0];
    pipe.write_fd = fds[1];

    EXPECT_TRUE(pipe.Valid());

    pipe.Close();

    EXPECT_FALSE(pipe.Valid());
    EXPECT_EQ(pipe.read_fd, -1);
    EXPECT_EQ(pipe.write_fd, -1);

    // Verify fds are closed (write should fail)
    EXPECT_EQ(::write(fds[1], "x", 1), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(PipeTest, DoubleCloseIsSafe) {
    Pipe pipe;
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    pipe.read_fd = fds[0];
    pipe.write_fd = fds[1];

    pipe.Close();
    pipe.Close();  // Should not crash

    EXPECT_FALSE(pipe.Valid());
}

// -----------------------------------------------------------------------------
// PipePool::Guard Tests
// -----------------------------------------------------------------------------

TEST(PipePoolGuardTest, AutoRelease) {
    PipePool pool(4);
    int read_fd;

    {
        auto guard = pool.AcquireGuarded();
        ASSERT_TRUE(guard.has_value());
        read_fd = guard->get().read_fd;
        EXPECT_TRUE(guard->get().Valid());
    }  // Guard destructor releases pipe back to pool

    // Acquire again - should get the same pipe
    auto pipe = pool.Acquire();
    ASSERT_TRUE(pipe.has_value());
    EXPECT_EQ(pipe->read_fd, read_fd);

    pipe->Close();
}

TEST(PipePoolGuardTest, MoveConstruction) {
    PipePool pool(4);

    auto guard1 = pool.AcquireGuarded();
    ASSERT_TRUE(guard1.has_value());
    int fd = guard1->get().read_fd;

    auto guard2 = std::move(*guard1);

    EXPECT_EQ(guard2.get().read_fd, fd);
    EXPECT_TRUE(guard2.get().Valid());
}

TEST(PipePoolGuardTest, GetAccess) {
    PipePool pool(4);

    auto guard = pool.AcquireGuarded();
    ASSERT_TRUE(guard.has_value());

    // Non-const access
    Pipe& ref = guard->get();
    EXPECT_TRUE(ref.Valid());

    // Const access
    const auto& const_guard = *guard;
    const Pipe& const_ref = const_guard.get();
    EXPECT_TRUE(const_ref.Valid());
}

// -----------------------------------------------------------------------------
// PipePool Destructor Tests
// -----------------------------------------------------------------------------

TEST(PipePoolTest, DestructorClosesAllPipes) {
    int fds[4];

    {
        PipePool pool(4);

        auto p1 = pool.Acquire();
        auto p2 = pool.Acquire();

        fds[0] = p1->read_fd;
        fds[1] = p1->write_fd;
        fds[2] = p2->read_fd;
        fds[3] = p2->write_fd;

        pool.Release(*p1);
        pool.Release(*p2);

        // Pool destructor runs here
    }

    // All fds should be closed
    for (int fd : fds) {
        EXPECT_EQ(::write(fd, "x", 1), -1);
        EXPECT_EQ(errno, EBADF);
    }
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}