#include "uring/core/fiber_io.hpp"
#include "uring/core/io.h"
#include "uring/extention/io_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace URing;

namespace
{
std::size_t current_thread_hash()
{
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

Result<void> write_and_read_back(FiberIO& fio, std::filesystem::path path, std::string_view payload,
                                 std::string& observed)
{
    FIBER_TRY(auto fd, fio.open(std::move(path), O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0644));

    FIBER_TRY(auto write_buf, fio.take_fixed_buffer(4096));
    std::memcpy(write_buf.ptr(), payload.data(), payload.size());

    FIBER_TRY(auto written, fio.write_fixed(fd, write_buf, payload.size(), 0));
    if (written != static_cast<int32_t>(payload.size()))
    {
        return error_from_errc(std::errc::io_error);
    }

    FIBER_TRY_VOID(fio.fsync(fd));

    FIBER_TRY(auto read_buf, fio.take_fixed_buffer(4096));
    std::memset(read_buf.ptr(), 0, read_buf.size());

    FIBER_TRY(auto read, fio.read_fixed(fd, read_buf, payload.size(), 0));
    if (read != static_cast<int32_t>(payload.size()))
    {
        return error_from_errc(std::errc::io_error);
    }

    observed.assign(reinterpret_cast<const char*>(read_buf.ptr()), static_cast<std::size_t>(read));

    FIBER_TRY_VOID(fio.close(std::move(fd)));
    return {};
}

std::filesystem::path temp_file_path()
{
    const auto name = std::string{"kio_fiber_io_test_"} + std::to_string(::getpid()) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / name;
}

Task<void> record_current_thread(std::atomic_size_t& thread_hash, std::atomic_bool& done)
{
    thread_hash.store(current_thread_hash(), std::memory_order_release);
    done.store(true, std::memory_order_release);
    co_return {};
}
}  // namespace

TEST(FiberIOTest, FixedBufferRoundTrip)
{
    IoOptions opts;
    opts.tick_timeout_ms = 1;

    IO io(0, nullptr, opts, {{4096, 4}});

    const auto path = temp_file_path();
    constexpr std::string_view payload = "fiber io round trip";

    std::atomic_bool done{false};
    std::optional<std::error_code> error;
    std::string observed;

    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
                   {
                       auto result = write_and_read_back(fio, path, payload, observed);
                       if (!result.has_value())
                       {
                           error = result.error();
                       }
                       done.store(true, std::memory_order_release);
                       return result;
                   },
        64 * 1024);

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    runner.request_stop();
    runner.join();

    std::filesystem::remove(path);

    ASSERT_TRUE(done.load(std::memory_order_acquire)) << "fiber did not finish before timeout";
    ASSERT_FALSE(error.has_value()) << error->message();
    EXPECT_EQ(observed, payload);
}

TEST(FiberIOTest, ScheduleFiberRunsOnTargetWorker)
{
    IoOptions opts;
    opts.tick_timeout_ms = 1;

    IoContext ctx(2, opts);

    const auto path = temp_file_path();
    std::atomic_bool worker_recorded{false};
    std::atomic_bool fiber_done{false};
    std::atomic_size_t worker_thread{0};
    std::atomic_size_t fiber_start_thread{0};
    std::atomic_size_t fiber_after_io_thread{0};
    std::optional<std::error_code> error;

    ctx.worker(1).schedule(record_current_thread(worker_thread, worker_recorded));
    ctx.worker(1).schedule_fiber(
        [&](FiberIO& fio) -> Result<void>
                                 {
                                     fiber_start_thread.store(current_thread_hash(), std::memory_order_release);

                                     auto fd_res = fio.open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0644);
                                     if (!fd_res)
                                     {
                                         error = fd_res.error();
                                         fiber_done.store(true, std::memory_order_release);
                                         return std::unexpected(fd_res.error());
                                     }

                                     auto close_res = fio.close(std::move(*fd_res));
                                     fiber_after_io_thread.store(current_thread_hash(), std::memory_order_release);
                                     if (!close_res)
                                     {
                                         error = close_res.error();
                                         fiber_done.store(true, std::memory_order_release);
                                         return std::unexpected(close_res.error());
                                     }

                                     fiber_done.store(true, std::memory_order_release);
                                     return {};
                                 },
        64 * 1024);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!worker_recorded.load(std::memory_order_acquire) || !fiber_done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ctx.join();
    std::filesystem::remove(path);

    ASSERT_TRUE(worker_recorded.load(std::memory_order_acquire)) << "target worker did not run marker task";
    ASSERT_TRUE(fiber_done.load(std::memory_order_acquire)) << "scheduled fiber did not finish before timeout";
    ASSERT_FALSE(error.has_value()) << error->message();
    EXPECT_EQ(fiber_start_thread.load(std::memory_order_acquire), worker_thread.load(std::memory_order_acquire));
    EXPECT_EQ(fiber_after_io_thread.load(std::memory_order_acquire), worker_thread.load(std::memory_order_acquire));
}

// Spawn N fibers on the same IO concurrently.  Exercises owned_fibers_ management
// and the interleaved suspend/resume of multiple live fibers.
TEST(FiberIOTest, MultipleConcurrentFibers)
{
    IoOptions opts;
    opts.tick_timeout_ms = 1;

    constexpr int kFiberCount = 8;
    IO io(0, nullptr, opts, {{4096, kFiberCount * 2}});

    std::atomic<int>       completed{0};
    std::vector<std::optional<std::error_code>> errors(kFiberCount);
    std::vector<std::string>                    results(kFiberCount);

    for (int i = 0; i < kFiberCount; ++i)
    {
        const auto path    = temp_file_path();
        const std::string  payload = "fiber_" + std::to_string(i);
        io.spawn_fiber(
            [&, i, path, payload](FiberIO& fio) mutable -> Result<void>
            {
                auto res = write_and_read_back(fio, path, payload, results[i]);
                if (!res)
                    errors[i] = res.error();
                std::filesystem::remove(path);
                completed.fetch_add(1, std::memory_order_release);
                return res;
            });
    }

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (completed.load(std::memory_order_acquire) < kFiberCount &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    runner.request_stop();
    runner.join();

    ASSERT_EQ(completed.load(), kFiberCount) << "not all fibers completed in time";
    for (int i = 0; i < kFiberCount; ++i)
    {
        EXPECT_FALSE(errors[i].has_value()) << "fiber " << i << ": " << errors[i]->message();
        EXPECT_EQ(results[i], "fiber_" + std::to_string(i));
    }
}

// A fiber that encounters an I/O error should propagate it via FIBER_TRY
// and return it as a Result error rather than crashing or hanging.
TEST(FiberIOTest, FiberPropagatesError)
{
    IoOptions opts;
    opts.tick_timeout_ms = 1;

    IO io(0, nullptr, opts, {});

    std::atomic_bool                   done{false};
    std::optional<std::error_code>     captured;

    io.spawn_fiber(
        [&](FiberIO& fio) -> Result<void>
        {
            // Opening a non-existent path without O_CREAT must return ENOENT.
            auto res = fio.open("/tmp/kio_no_such_file_does_not_exist_xyz", O_RDONLY);
            captured = res.has_value() ? std::nullopt : std::optional{res.error()};
            done.store(true, std::memory_order_release);
            if (!res)
                return std::unexpected(res.error());
            FIBER_TRY_VOID(fio.close(std::move(*res)));
            return {};
        });

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    runner.request_stop();
    runner.join();

    ASSERT_TRUE(done.load()) << "fiber did not finish before timeout";
    ASSERT_TRUE(captured.has_value()) << "expected an error but none was captured";
    EXPECT_EQ(captured->value(), ENOENT);
}

// schedule_fiber() from an external thread exercises the FiberQueue path directly.
// The fiber must run on the IO thread, not the calling thread.
TEST(FiberIOTest, ScheduleFiberFromExternalThread)
{
    IoOptions opts;
    opts.tick_timeout_ms = 1;

    IO io(0, nullptr, opts, {});

    std::atomic_bool   done{false};
    std::atomic_size_t fiber_thread{0};
    std::atomic_size_t io_thread{0};

    std::jthread runner(
        [&](std::stop_token st)
        {
            io_thread.store(current_thread_hash(), std::memory_order_release);
            io.run_blocking(st);
        });

    // Wait until the IO thread is up.
    while (io_thread.load(std::memory_order_acquire) == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // schedule_fiber from THIS (test) thread — goes through FiberQueue + wake().
    io.schedule_fiber(
        [&](FiberIO& /*fio*/) -> Result<void>
        {
            fiber_thread.store(current_thread_hash(), std::memory_order_release);
            done.store(true, std::memory_order_release);
            return {};
        });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    runner.request_stop();
    runner.join();

    ASSERT_TRUE(done.load()) << "scheduled fiber did not finish before timeout";
    // Fiber must have run on the IO thread, not on the test thread.
    EXPECT_NE(fiber_thread.load(), current_thread_hash());
    EXPECT_EQ(fiber_thread.load(), io_thread.load());
}
