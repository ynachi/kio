#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#define BOOST_ASIO_HAS_IO_URING
#define BOOST_ASIO_HAS_FILE
#define BOOST_ERROR_CODE_HEADER_ONLY
#define BOOST_SYSTEM_NO_DEPRECATED
#include <boost/asio.hpp>
#include <boost/asio/random_access_file.hpp>
#include <boost/asio/write_at.hpp>

#include "uring/core/fiber_io.hpp"
#include "uring/core/io.h"

namespace
{
namespace asio = boost::asio;
using Clock = std::chrono::steady_clock;

struct Options
{
    std::filesystem::path path = "/tmp/kio-asio-disk-bench.dat";
    std::uint64_t bytes = 1024ull * 1024ull * 1024ull;
    std::size_t block = 64ull * 1024ull;
};

std::uint64_t parse_size(std::string_view s)
{
    std::uint64_t multiplier = 1;
    if (!s.empty())
    {
        const char suffix = s.back();
        if (suffix == 'k' || suffix == 'K')
        {
            multiplier = 1024;
            s.remove_suffix(1);
        }
        else if (suffix == 'm' || suffix == 'M')
        {
            multiplier = 1024ull * 1024ull;
            s.remove_suffix(1);
        }
        else if (suffix == 'g' || suffix == 'G')
        {
            multiplier = 1024ull * 1024ull * 1024ull;
            s.remove_suffix(1);
        }
    }
    return std::stoull(std::string{s}) * multiplier;
}

Options parse_args(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg.starts_with("--path="))
        {
            opts.path = arg.substr(std::strlen("--path="));
        }
        else if (arg.starts_with("--bytes="))
        {
            opts.bytes = parse_size(arg.substr(std::strlen("--bytes=")));
        }
        else if (arg.starts_with("--block="))
        {
            opts.block = static_cast<std::size_t>(parse_size(arg.substr(std::strlen("--block="))));
        }
        else
        {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    if (opts.block == 0 || opts.bytes == 0)
    {
        throw std::invalid_argument("bytes and block must be positive");
    }
    return opts;
}

void prepare_file(const std::filesystem::path& path, std::uint64_t bytes)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        throw std::runtime_error("open failed: " + std::string(std::strerror(errno)));
    }

    const int fallocate_res = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes));
    if (fallocate_res != 0)
    {
        ::close(fd);
        throw std::runtime_error("posix_fallocate failed: " + std::string(std::strerror(fallocate_res)));
    }

    if (::close(fd) != 0)
    {
        throw std::runtime_error("close failed: " + std::string(std::strerror(errno)));
    }
}

struct BenchResult
{
    std::string name;
    double seconds{};
    double mib_per_sec{};
};

void print_result(const BenchResult& r)
{
    std::cout << "  " << r.name;
    for (std::size_t i = r.name.size(); i < 16; ++i)
    {
        std::cout << ' ';
    }
    std::cout << r.seconds << "s  " << r.mib_per_sec << " MiB/s\n";
}

URing::Task<void> kio_write_all(URing::IO& io, URing::Fd& fd, std::span<const std::byte> block, std::uint64_t bytes)
{
    std::uint64_t offset = 0;
    while (offset < bytes)
    {
        const auto len = std::min<std::uint64_t>(block.size(), bytes - offset);
        const auto res = co_await io.write(fd, block.first(static_cast<std::size_t>(len)), static_cast<off_t>(offset));
        if (!res)
        {
            throw std::runtime_error("kio write failed: " + res.error().message());
        }
        if (*res != static_cast<int>(len))
        {
            throw std::runtime_error("kio short write");
        }
        offset += len;
    }
    co_return URing::Result<void>{};
}

BenchResult bench_kio(const Options& opts, const std::vector<std::byte>& block)
{
    const auto path = opts.path.string() + ".kio";
    prepare_file(path, opts.bytes);

    const int raw_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (raw_fd < 0)
    {
        throw std::runtime_error("open kio file failed: " + std::string(std::strerror(errno)));
    }

    URing::Fd fd{raw_fd};
    URing::IO io{0};

    const auto start = Clock::now();
    auto res = URing::sync_wait(io, kio_write_all(io, fd, block, opts.bytes));
    const auto end = Clock::now();
    if (!res)
    {
        throw std::runtime_error("kio write coroutine failed: " + res.error().message());
    }

    const double seconds = std::chrono::duration<double>(end - start).count();
    return {"kio", seconds, static_cast<double>(opts.bytes) / (1024.0 * 1024.0) / seconds};
}

URing::Result<void> kio_fiber_write_all(URing::FiberIO& fio, URing::Fd& fd, std::span<const std::byte> block,
                                        std::uint64_t bytes)
{
    std::uint64_t offset = 0;
    while (offset < bytes)
    {
        const auto len = std::min<std::uint64_t>(block.size(), bytes - offset);
        iovec iov{.iov_base = const_cast<std::byte*>(block.data()), .iov_len = static_cast<std::size_t>(len)};
        FIBER_TRY(auto written, fio.writev(fd, std::span<const iovec>{&iov, 1}, static_cast<off_t>(offset)));
        if (written != static_cast<int32_t>(len))
        {
            return URing::error_from_errc(std::errc::io_error);
        }
        offset += len;
    }
    return {};
}

BenchResult bench_kio_fiber(const Options& opts, const std::vector<std::byte>& block)
{
    const auto path = opts.path.string() + ".kio-fiber";
    prepare_file(path, opts.bytes);

    const int raw_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (raw_fd < 0)
    {
        throw std::runtime_error("open kio fiber file failed: " + std::string(std::strerror(errno)));
    }

    URing::Fd fd{raw_fd};
    URing::IO io{0};
    std::atomic_bool done{false};
    std::optional<std::error_code> error;
    BenchResult result;

    io.spawn_fiber(
        [&](URing::FiberIO& fio) -> URing::Result<void>
                   {
                       const auto start = Clock::now();
                       auto write_res = kio_fiber_write_all(fio, fd, block, opts.bytes);
                       const auto end = Clock::now();

                       if (!write_res)
                       {
                           error = write_res.error();
                       }
                       else
                       {
                           const double seconds = std::chrono::duration<double>(end - start).count();
                           result = {"kio-fiber", seconds,
                                     static_cast<double>(opts.bytes) / (1024.0 * 1024.0) / seconds};
                       }
                       done.store(true, std::memory_order_release);
                       return write_res;
                   },
        64 * 1024);

    std::jthread runner([&](std::stop_token st) { io.run_blocking(st); });
    while (!done.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    runner.request_stop();
    runner.join();

    if (error)
    {
        throw std::runtime_error("kio fiber write failed: " + error->message());
    }
    return result;
}

BenchResult bench_asio(const Options& opts, const std::vector<std::byte>& block)
{
    const auto path = opts.path.string() + ".asio";
    prepare_file(path, opts.bytes);

    asio::io_context io;
    auto guard = asio::make_work_guard(io);
    std::jthread runner([&] { io.run(); });

    asio::random_access_file file(io);
    file.open(path, asio::file_base::write_only);

    const auto start = Clock::now();
    std::uint64_t offset = 0;
    while (offset < opts.bytes)
    {
        const auto len = std::min<std::uint64_t>(block.size(), opts.bytes - offset);
        auto fut = asio::async_write_at(file, offset, asio::buffer(block.data(), static_cast<std::size_t>(len)),
                                        asio::use_future);
        const auto written = fut.get();
        if (written != len)
        {
            throw std::runtime_error("asio short write");
        }
        offset += len;
    }
    const auto end = Clock::now();

    boost::system::error_code ec;
    file.close(ec);
    guard.reset();
    io.stop();
    runner.join();

    const double seconds = std::chrono::duration<double>(end - start).count();
    return {"asio", seconds, static_cast<double>(opts.bytes) / (1024.0 * 1024.0) / seconds};
}

}  // namespace

int main(int argc, char** argv)
{
    try
    {
        const auto opts = parse_args(argc, argv);
        std::vector<std::byte> block(opts.block);
        for (std::size_t i = 0; i < block.size(); ++i)
        {
            block[i] = static_cast<std::byte>(i);
        }

        std::cout << "bytes=" << opts.bytes << " block=" << opts.block << '\n';
        print_result(bench_kio(opts, block));
        print_result(bench_kio_fiber(opts, block));
        print_result(bench_asio(opts, block));
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
