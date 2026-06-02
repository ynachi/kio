#include "../io_context.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    std::filesystem::path path = "/tmp/zio-disk-bench.dat";
    std::uint64_t bytes = 1024ull * 1024ull * 1024ull;
    std::size_t block = 64ull * 1024ull;
    std::size_t fibers = 16;
    std::size_t stack_size = 64 * 1024;
};

struct BenchResult
{
    std::string name;
    double seconds{};
    double mib_per_sec{};
};

struct Fd
{
    int fd = -1;

    explicit Fd(int value) noexcept : fd(value) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    ~Fd()
    {
        if (fd >= 0)
            ::close(fd);
    }
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
            opts.path = arg.substr(std::strlen("--path="));
        else if (arg.starts_with("--bytes="))
            opts.bytes = parse_size(arg.substr(std::strlen("--bytes=")));
        else if (arg.starts_with("--block="))
            opts.block = static_cast<std::size_t>(parse_size(arg.substr(std::strlen("--block="))));
        else if (arg.starts_with("--fibers="))
            opts.fibers = static_cast<std::size_t>(parse_size(arg.substr(std::strlen("--fibers="))));
        else if (arg.starts_with("--stack-size="))
            opts.stack_size = static_cast<std::size_t>(parse_size(arg.substr(std::strlen("--stack-size="))));
        else
            throw std::invalid_argument("unknown argument: " + arg);
    }

    if (opts.block == 0 || opts.bytes == 0 || opts.fibers == 0 || opts.stack_size == 0)
        throw std::invalid_argument("bytes, block, fibers, and stack-size must be positive");
    return opts;
}

void prepare_file(const std::filesystem::path& path, std::uint64_t bytes)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0)
        throw std::runtime_error("open failed: " + std::string(std::strerror(errno)));

    const int fallocate_res = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes));
    if (fallocate_res != 0)
    {
        ::close(fd);
        throw std::runtime_error("posix_fallocate failed: " + std::string(std::strerror(fallocate_res)));
    }

    if (::close(fd) != 0)
        throw std::runtime_error("close failed: " + std::string(std::strerror(errno)));
}

void print_result(const BenchResult& r)
{
    std::cout << "  " << r.name;
    for (std::size_t i = r.name.size(); i < 16; ++i)
        std::cout << ' ';
    std::cout << r.seconds << "s  " << r.mib_per_sec << " MiB/s\n";
}

zio::Result<> write_file(zio::io_context& ctx, int fd, const Options& opts,
                         const std::vector<std::byte>& block)
{
    std::uint64_t offset = 0;
    while (offset < opts.bytes)
    {
        const auto len = std::min<std::uint64_t>(block.size(), opts.bytes - offset);
        ZIO_TRY_LOG(auto written,
                    ctx.write(fd, std::span{block.data(), static_cast<std::size_t>(len)},
                              static_cast<off_t>(offset)),
                    "zio write offset={} len={}", offset, len);
        if (written != len)
            return zio::error_from_errc(std::errc::io_error);
        offset += len;
    }

    return {};
}

zio::Result<BenchResult> bench_zio(const Options& opts, const std::vector<std::byte>& block)
{
    const auto path = opts.path.string() + ".zio";
    prepare_file(path, opts.bytes);

    const int raw_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (raw_fd < 0)
        throw std::runtime_error("open zio file failed: " + std::string(std::strerror(errno)));
    Fd fd{raw_fd};

    zio::io_context ctx{zio::io_context_options{
        .ring_entries = 4096,
        .ring_flags = 0,
        .fiber_count = opts.fibers,
        .stack_size = opts.stack_size,
        .ready_budget = 256,
    }};

    BenchResult result;
    zio::Result<> write_res;
    bool completed = false;

    auto spawn_res = ctx.spawn(
        [&]
        {
            const auto start = Clock::now();

            write_res = write_file(ctx, fd.fd, opts, block);
            if (!write_res)
                return;

            const auto end = Clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();
            result = {"zio", seconds, static_cast<double>(opts.bytes) / (1024.0 * 1024.0) / seconds};
            completed = true;
        });
    if (!spawn_res)
        return std::unexpected(spawn_res.error());

    ZIO_TRY_VOID_LOG(ctx.run(), "zio context run");
    ZIO_TRY_VOID(write_res);
    if (!completed)
        return zio::error_from_errc(std::errc::io_error);
    return result;
}
}  // namespace

int main(int argc, char** argv)
{
    try
    {
        const auto opts = parse_args(argc, argv);
        std::vector<std::byte> block(opts.block);
        for (std::size_t i = 0; i < block.size(); ++i)
            block[i] = static_cast<std::byte>(i);

        std::cout << "bytes=" << opts.bytes << " block=" << opts.block << '\n';
        auto zio_result = bench_zio(opts, block);
        if (!zio_result)
            throw std::runtime_error("zio benchmark failed: " + zio_result.error().message());
        print_result(*zio_result);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
