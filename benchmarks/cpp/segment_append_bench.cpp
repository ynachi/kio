#include "bitcask/segment.hpp"
#include "uring/core/io.h"
#include "uring/logger.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/uio.h>

namespace fs = std::filesystem;

namespace
{
struct Options
{
    size_t entries = 100'000;
    size_t key_size = 16;
    size_t value_size = 128;
    fs::path dir = fs::temp_directory_path() / "kio_segment_append_bench";
};

size_t parse_size(std::string_view text)
{
    size_t value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            throw std::invalid_argument("expected unsigned integer");
        }
        value = value * 10 + static_cast<size_t>(c - '0');
    }
    return value;
}

Options parse_args(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        auto read_value = [&](std::string_view name) -> std::string_view
        {
            if (!arg.starts_with(name) || arg.size() <= name.size() || arg[name.size()] != '=')
            {
                return {};
            }
            return arg.substr(name.size() + 1);
        };

        if (auto value = read_value("--entries"); !value.empty())
        {
            opts.entries = parse_size(value);
        }
        else if (auto value = read_value("--key-size"); !value.empty())
        {
            opts.key_size = parse_size(value);
        }
        else if (auto value = read_value("--value-size"); !value.empty())
        {
            opts.value_size = parse_size(value);
        }
        else if (auto value = read_value("--dir"); !value.empty())
        {
            opts.dir = value;
        }
        else
        {
            throw std::invalid_argument(std::format("unknown argument: {}", arg));
        }
    }
    return opts;
}

std::string make_payload(size_t size, char seed)
{
    std::string out(size, seed);
    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<char>('a' + ((i + static_cast<unsigned char>(seed)) % 26));
    }
    return out;
}

uint64_t segment_bytes(const Options& opts)
{
    const uint64_t entry_size = sizeof(bitcask::LogEntryHeader) + opts.key_size + opts.value_size + sizeof(uint64_t);
    return entry_size * opts.entries;
}

template <typename Fn>
double measure_seconds(Fn&& fn)
{
    const auto start = std::chrono::steady_clock::now();
    fn();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - start).count();
}

void print_result(std::string_view name, const Options& opts, double seconds, uint64_t bytes)
{
    const double ops_s = static_cast<double>(opts.entries) / seconds;
    const double mib_s = static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
    std::cout << std::format("{:<18} {:>10.3f}s {:>12.0f} ops/s {:>10.1f} MiB/s\n", name, seconds, ops_s, mib_s);
}

URing::Task<void> run_segment_append(URing::IO& io, bitcask::SegmentManager& manager, const Options& opts,
                                     std::string key, std::string value)
{
    for (size_t i = 0; i < opts.entries; ++i)
    {
        key[0] = static_cast<char>('a' + (i % 26));
        value[0] = static_cast<char>('A' + (i % 26));

        auto res = co_await manager.append(io, key, value);
        if (!res.has_value())
        {
            co_return std::unexpected(res.error());
        }
    }

    co_return {};
}

void bench_segment_manager(const Options& opts)
{
    fs::path dir = opts.dir / "segment_manager";
    fs::remove_all(dir);
    fs::create_directories(dir / "partition_0");

    URing::IO io{0};
    bitcask::BitcaskConfig cfg;
    cfg.directory = dir;
    cfg.max_segment_size = segment_bytes(opts) + 4096;

    bitcask::SegmentManager manager{0, cfg, 0, 0};
    std::string key = make_payload(opts.key_size, 'k');
    std::string value = make_payload(opts.value_size, 'v');

    const double seconds = measure_seconds(
        [&]
        {
            auto res = URing::sync_wait(io, run_segment_append(io, manager, opts, key, value));
            if (!res.has_value())
            {
                throw std::system_error(res.error());
            }
        });

    auto close_res = URing::sync_wait(io, manager.close(io));
    if (!close_res.has_value())
    {
        throw std::system_error(close_res.error());
    }

    print_result("SegmentManager", opts, seconds, segment_bytes(opts));
}

void bench_raw_pwritev(const Options& opts)
{
    fs::path dir = opts.dir / "raw_pwritev";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path path = dir / "data.db";

    int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_EXCL, 0644);
    if (fd < 0)
    {
        throw std::system_error(errno, std::system_category(), "open");
    }

    const uint64_t total_bytes = segment_bytes(opts);
    if (::posix_fallocate(fd, 0, static_cast<off_t>(total_bytes + 4096)) != 0)
    {
        const int err = errno;
        ::close(fd);
        throw std::system_error(err, std::system_category(), "posix_fallocate");
    }

    std::string key = make_payload(opts.key_size, 'k');
    std::string value = make_payload(opts.value_size, 'v');
    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr)
    {
        ::close(fd);
        throw std::bad_alloc();
    }

    off_t offset = 0;
    uint64_t seq = 0;

    const double seconds = measure_seconds(
        [&]
        {
            for (size_t i = 0; i < opts.entries; ++i)
            {
                key[0] = static_cast<char>('a' + (i % 26));
                value[0] = static_cast<char>('A' + (i % 26));

                bitcask::LogEntryHeader hdr{};
                hdr.seq_num = ++seq;
                hdr.val_len = static_cast<uint32_t>(value.size());
                hdr.key_len = static_cast<uint16_t>(key.size());
                hdr.flags = static_cast<uint16_t>(bitcask::EntryFlags::HasValue);
                hdr.hdr_crc = XXH3_64bits(&hdr.seq_num, 24);

                XXH3_64bits_reset(state);
                XXH3_64bits_update(state, key.data(), key.size());
                XXH3_64bits_update(state, value.data(), value.size());
                uint64_t payload_crc = XXH3_64bits_digest(state);

                std::array<iovec, 4> iovs = {
                    {{&hdr, sizeof(hdr)},
                     {key.data(), key.size()},
                     {value.data(), value.size()},
                     {&payload_crc, sizeof(payload_crc)}}};

                const auto written = ::pwritev(fd, iovs.data(), static_cast<int>(iovs.size()), offset);
                if (written < 0)
                {
                    throw std::system_error(errno, std::system_category(), "pwritev");
                }
                if (written != static_cast<ssize_t>(hdr.total_size()))
                {
                    throw std::runtime_error("short pwritev");
                }
                offset += written;
            }
        });

    XXH3_freeState(state);
    if (::ftruncate(fd, offset) != 0)
    {
        const int err = errno;
        ::close(fd);
        throw std::system_error(err, std::system_category(), "ftruncate");
    }
    ::close(fd);

    print_result("raw pwritev", opts, seconds, total_bytes);
}
}  // namespace

int main(int argc, char** argv)
{
    try
    {
        URing::ALOG::set_level(URing::ALOG::Level::Disabled);

        const Options opts = parse_args(argc, argv);
        fs::remove_all(opts.dir);
        fs::create_directories(opts.dir);

        std::cout << std::format("entries={} key={} value={} bytes={}\n", opts.entries, opts.key_size, opts.value_size,
                                 segment_bytes(opts));

        bench_segment_manager(opts);
        bench_raw_pwritev(opts);
        fs::remove_all(opts.dir);
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
