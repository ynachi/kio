#include "../other_baseline.hpp.cpp"

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
namespace io = iouring_coro;
using Clock = std::chrono::steady_clock;

struct Options
{
    std::filesystem::path path = "/tmp/other-baseline-disk-bench.dat";
    std::uint64_t bytes = 1024ull * 1024ull * 1024ull;
    std::size_t block = 64ull * 1024ull;
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
        else
            throw std::invalid_argument("unknown argument: " + arg);
    }

    if (opts.block == 0 || opts.bytes == 0)
        throw std::invalid_argument("bytes and block must be positive");
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
    for (std::size_t i = r.name.size(); i < 20; ++i)
        std::cout << ' ';
    std::cout << r.seconds << "s  " << r.mib_per_sec << " MiB/s\n";
}

BenchResult bench_other_baseline(const Options& opts, const std::vector<std::byte>& block)
{
    const auto path = opts.path.string() + ".other-baseline";
    prepare_file(path, opts.bytes);

    const int raw_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (raw_fd < 0)
        throw std::runtime_error("open baseline file failed: " + std::string(std::strerror(errno)));
    Fd fd{raw_fd};

    io::scheduler sched{4096};
    BenchResult result;

    sched.spawn(
        [&]
        {
            const auto start = Clock::now();

            std::uint64_t offset = 0;
            while (offset < opts.bytes)
            {
                const auto len = std::min<std::uint64_t>(block.size(), opts.bytes - offset);
                auto written = io::write(fd.fd, std::span{block.data(), static_cast<std::size_t>(len)}, offset);
                if (!written)
                    throw std::runtime_error("baseline write failed: " + written.error().message());
                if (*written != len)
                    throw std::runtime_error("baseline short write");
                offset += len;
            }

            const auto end = Clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();
            result = {"other-baseline", seconds,
                      static_cast<double>(opts.bytes) / (1024.0 * 1024.0) / seconds};
        });

    sched.run();
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
        print_result(bench_other_baseline(opts, block));
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
