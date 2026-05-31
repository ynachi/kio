#include "uring/core/fiber_io.hpp"
#include "uring/core/io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    std::size_t iterations = 1'000'000;
    std::size_t size = 256;
    std::size_t warmup = 50'000;
    std::string path = "/dev/null";
};

struct BenchResult
{
    std::string name;
    std::size_t iterations{};
    double seconds{};
    double ops_per_sec{};
    double ns_per_op{};
};

std::size_t parse_size(std::string_view value)
{
    std::uint64_t multiplier = 1;
    if (!value.empty())
    {
        const char suffix = value.back();
        if (suffix == 'k' || suffix == 'K')
        {
            multiplier = 1'000;
            value.remove_suffix(1);
        }
        else if (suffix == 'm' || suffix == 'M')
        {
            multiplier = 1'000'000;
            value.remove_suffix(1);
        }
    }
    return static_cast<std::size_t>(std::stoull(std::string{value}) * multiplier);
}

Options parse_args(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg.starts_with("--iterations="))
        {
            opts.iterations = parse_size(arg.substr(std::strlen("--iterations=")));
        }
        else if (arg.starts_with("--size="))
        {
            opts.size = parse_size(arg.substr(std::strlen("--size=")));
        }
        else if (arg.starts_with("--warmup="))
        {
            opts.warmup = parse_size(arg.substr(std::strlen("--warmup=")));
        }
        else if (arg.starts_with("--path="))
        {
            opts.path = arg.substr(std::strlen("--path="));
        }
        else
        {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    if (opts.iterations == 0 || opts.size == 0)
    {
        throw std::invalid_argument("iterations and size must be positive");
    }
    return opts;
}

void print_result(const BenchResult& result)
{
    std::cout << std::format("{:<12} {:>10} ops  {:>8.4f}s  {:>12.0f} ops/s  {:>8.1f} ns/op\n", result.name,
                             result.iterations, result.seconds, result.ops_per_sec, result.ns_per_op);
}

URing::Fd open_sink(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        throw std::runtime_error("open failed: " + std::string{std::strerror(errno)});
    }
    return URing::Fd{fd};
}

URing::Result<void> check_write_result(int32_t written, std::size_t expected)
{
    if (written < 0)
    {
        return std::unexpected(URing::make_error_code(written));
    }
    if (written != static_cast<int32_t>(expected))
    {
        return URing::error_from_errc(std::errc::io_error);
    }
    return {};
}

URing::Task<void> stackless_write_loop(URing::IO& io, URing::Fd& fd, const Options& opts, BenchResult& out,
                                       std::atomic_bool& done)
{
    URING_TRY(auto buf, io.take_fixed_buffer(opts.size));
    std::fill(buf.data().begin(), buf.data().end(), std::byte{0x5a});

    for (std::size_t i = 0; i < opts.warmup; ++i)
    {
        URING_TRY(auto written, co_await io.write_fixed(fd, buf, opts.size));
        URING_TRY_VOID(check_write_result(written, opts.size));
    }

    const auto start = Clock::now();
    for (std::size_t i = 0; i < opts.iterations; ++i)
    {
        URING_TRY(auto written, co_await io.write_fixed(fd, buf, opts.size));
        URING_TRY_VOID(check_write_result(written, opts.size));
    }
    const auto end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    out = {.name = "stackless",
           .iterations = opts.iterations,
           .seconds = seconds,
           .ops_per_sec = static_cast<double>(opts.iterations) / seconds,
           .ns_per_op = seconds * 1'000'000'000.0 / static_cast<double>(opts.iterations)};
    done.store(true, std::memory_order_release);
    co_return URing::Result<void>{};
}

BenchResult bench_stackless(const Options& opts)
{
    URing::IO io{0, nullptr, {}, {{4096, 2}}};
    auto fd = open_sink(opts.path);

    BenchResult out;
    std::atomic_bool done{false};
    std::optional<std::error_code> error;

    io.schedule(
        [&]() -> URing::Task<void>
        {
            auto result = co_await stackless_write_loop(io, fd, opts, out, done);
            if (!result.has_value())
            {
                error = result.error();
                done.store(true, std::memory_order_release);
            }
            co_return URing::Result<void>{};
        }());

    while (!done.load(std::memory_order_acquire))
    {
        io.run_once();
    }

    if (error.has_value())
    {
        throw std::runtime_error("stackless failed: " + error->message());
    }
    return out;
}

URing::Result<void> stackful_write_loop(URing::FiberIO& fio, URing::Fd& fd, const Options& opts, BenchResult& out)
{
    FIBER_TRY(auto buf, fio.take_fixed_buffer(opts.size));
    std::fill(buf.data().begin(), buf.data().end(), std::byte{0x5a});

    for (std::size_t i = 0; i < opts.warmup; ++i)
    {
        FIBER_TRY(auto written, fio.write_fixed(fd, buf, opts.size));
        FIBER_TRY_VOID(check_write_result(written, opts.size));
    }

    const auto start = Clock::now();
    for (std::size_t i = 0; i < opts.iterations; ++i)
    {
        FIBER_TRY(auto written, fio.write_fixed(fd, buf, opts.size));
        FIBER_TRY_VOID(check_write_result(written, opts.size));
    }
    const auto end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    out = {.name = "stackful",
           .iterations = opts.iterations,
           .seconds = seconds,
           .ops_per_sec = static_cast<double>(opts.iterations) / seconds,
           .ns_per_op = seconds * 1'000'000'000.0 / static_cast<double>(opts.iterations)};
    return {};
}

BenchResult bench_stackful(const Options& opts)
{
    URing::IO io{0, nullptr, {}, {{4096, 2}}};
    auto fd = open_sink(opts.path);

    BenchResult out;
    std::atomic_bool done{false};
    std::optional<std::error_code> error;

    io.spawn_fiber(64 * 1024,
                   [&](URing::FiberIO& fio) -> URing::Result<void>
                   {
                       auto result = stackful_write_loop(fio, fd, opts, out);
                       if (!result.has_value())
                       {
                           error = result.error();
                       }
                       done.store(true, std::memory_order_release);
                       return result;
                   });

    while (!done.load(std::memory_order_acquire))
    {
        io.run_once();
    }

    if (error.has_value())
    {
        throw std::runtime_error("stackful failed: " + error->message());
    }
    return out;
}
}  // namespace

int main(int argc, char** argv)
{
    try
    {
        const auto opts = parse_args(argc, argv);
        std::cout << "path=" << opts.path << " iterations=" << opts.iterations << " warmup=" << opts.warmup
                  << " size=" << opts.size << '\n';

        const auto stackless = bench_stackless(opts);
        const auto stackful = bench_stackful(opts);

        print_result(stackless);
        print_result(stackful);

        const double ratio = stackful.ops_per_sec / stackless.ops_per_sec;
        std::cout << std::format("stackful/stackless throughput ratio: {:.3f}x\n", ratio);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
