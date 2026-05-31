#include "uring/core/fiber_io.hpp"
#include "uring/core/io.h"

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

    io.spawn_fiber(64 * 1024,
                   [&](FiberIO& fio) -> Result<void>
                   {
                       auto result = write_and_read_back(fio, path, payload, observed);
                       if (!result.has_value())
                       {
                           error = result.error();
                       }
                       done.store(true, std::memory_order_release);
                       return result;
                   });

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
