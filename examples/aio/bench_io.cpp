#include "kio/kio.hpp"
#include "kio/core/io_helpers.hpp"
#include "kio/core/task_group.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr std::size_t kBlockSize = 4 * 1024;
constexpr std::size_t kDepth = 64;
constexpr std::size_t kTotalSize = 256 * 1024 * 1024;
constexpr std::size_t kTotalOps = kTotalSize / kBlockSize;
constexpr std::size_t kSyncTotalSize = 64 * 1024 * 1024;
constexpr std::size_t kSyncTotalOps = kSyncTotalSize / kBlockSize;
constexpr std::size_t kSyncEvery = 64;
constexpr std::uint32_t kRingSize = 256;

struct BenchResult {
    std::string name;
    std::size_t ops;
    std::size_t bytes;
    double ops_per_sec;
    double mib_per_sec;
};

double ToSeconds(const std::chrono::steady_clock::duration elapsed) {
    const auto secs = std::chrono::duration<double>(elapsed).count();
    return secs > 0.0 ? secs : 0.0;
}

BenchResult MakeBenchResult(const std::string& name, std::size_t ops, std::size_t bytes,
                            const std::chrono::steady_clock::duration elapsed) {
    const double secs = ToSeconds(elapsed);
    return {
        .name = name,
        .ops = ops,
        .bytes = bytes,
        .ops_per_sec = secs > 0.0 ? static_cast<double>(ops) / secs : 0.0,
        .mib_per_sec = secs > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / secs : 0.0,
    };
}

std::filesystem::path MakeTempPath(const char* prefix) {
    const char* base = std::getenv("BENCH_DIR");
    const auto dir = (base != nullptr && base[0] != '\0')
        ? std::filesystem::path(base)
        : std::filesystem::temp_directory_path();
    return dir /
           std::format("{}-{}-{}.bin", prefix, ::getpid(),
                       std::chrono::steady_clock::now().time_since_epoch().count());
}

void PrintResult(const BenchResult& r) {
    std::cout << std::left << std::setw(32) << r.name << " "
              << std::fixed << std::setprecision(0) << r.ops_per_sec << " ops/s "
              << std::setprecision(1) << r.mib_per_sec << " MiB/s\n";
}

kio::Task<kio::Result<void>> SeedFile(kio::IoContext& ctx, int fd, std::span<const std::byte> buf) {
    for (std::size_t i = 0; i < kTotalOps; ++i) {
        auto res = co_await kio::AsyncWriteExact(ctx, fd, buf, i * kBlockSize);
        if (!res) {
            co_return std::unexpected(res.error());
        }
    }
    co_return {};
}

kio::Task<kio::Result<BenchResult>> BenchSeqWrite(kio::IoContext& ctx, const std::filesystem::path path) {
    const auto fd_res = co_await kio::AsyncOpen(ctx, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (!fd_res) {
        co_return std::unexpected(fd_res.error());
    }
    const int fd = fd_res->Get();

    std::vector<std::byte> buf(kBlockSize, std::byte{0x5a});
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kTotalOps; ++i) {
        auto res = co_await kio::AsyncWriteExact(ctx, fd, buf, i * kBlockSize);
        if (!res) {
            co_return std::unexpected(res.error());
        }
    }
    const auto elapsed = start - start + (std::chrono::steady_clock::now() - start);

    co_return MakeBenchResult("kio seq write 4KiB", kTotalOps, kTotalSize, elapsed);
}

kio::Task<kio::Result<BenchResult>> BenchSeqRead(kio::IoContext& ctx, const std::filesystem::path path) {
    const auto fd_res = co_await kio::AsyncOpen(ctx, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (!fd_res) {
        co_return std::unexpected(fd_res.error());
    }
    const int fd = fd_res->Get();

    std::vector<std::byte> seed(kBlockSize, std::byte{0x31});
    auto seeded = co_await SeedFile(ctx, fd, seed);
    if (!seeded) {
        co_return std::unexpected(seeded.error());
    }

    std::vector<std::byte> buf(kBlockSize);
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kTotalOps; ++i) {
        auto res = co_await kio::AsyncReadExact(ctx, fd, buf, i * kBlockSize);
        if (!res) {
            co_return std::unexpected(res.error());
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    co_return MakeBenchResult("kio seq read 4KiB", kTotalOps, kTotalSize, elapsed);
}

kio::Task<> OneWrite(kio::IoContext& ctx, int fd, std::span<const std::byte> buf, std::uint64_t offset,
                     std::optional<std::error_code>& first_error) {
    auto res = co_await kio::AsyncWriteExact(ctx, fd, buf, offset);
    if (!res && !first_error.has_value()) {
        first_error = res.error();
    }
}

kio::Task<> OneRead(kio::IoContext& ctx, int fd, std::span<std::byte> buf, std::uint64_t offset,
                    std::optional<std::error_code>& first_error) {
    auto res = co_await kio::AsyncReadExact(ctx, fd, buf, offset);
    if (!res && !first_error.has_value()) {
        first_error = res.error();
    }
}

kio::Task<kio::Result<BenchResult>> BenchStreamingWrite(kio::IoContext& ctx, const std::filesystem::path path) {
    const auto fd_res = co_await kio::AsyncOpen(ctx, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (!fd_res) {
        co_return std::unexpected(fd_res.error());
    }
    const int fd = fd_res->Get();

    std::vector<std::vector<std::byte>> buffers(kDepth, std::vector<std::byte>(kBlockSize));
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t round = 0; round < kTotalOps / kDepth; ++round) {
        const std::size_t base = round * kDepth;
        std::optional<std::error_code> first_error;
        kio::TaskGroup<> group(kDepth);
        for (std::size_t i = 0; i < kDepth; ++i) {
            std::fill(buffers[i].begin(), buffers[i].end(), std::byte{static_cast<unsigned char>((base + i) & 0xff)});
            group.Spawn(OneWrite(ctx, fd, buffers[i], (base + i) * kBlockSize, first_error));
        }
        co_await group.JoinAll(ctx);
        if (first_error) {
            co_return std::unexpected(*first_error);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    co_return MakeBenchResult("kio streaming write depth64", kTotalOps, kTotalSize, elapsed);
}

kio::Task<kio::Result<BenchResult>> BenchStreamingRead(kio::IoContext& ctx, const std::filesystem::path path) {
    const auto fd_res = co_await kio::AsyncOpen(ctx, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (!fd_res) {
        co_return std::unexpected(fd_res.error());
    }
    const int fd = fd_res->Get();

    std::vector<std::byte> seed(kBlockSize, std::byte{0x41});
    auto seeded = co_await SeedFile(ctx, fd, seed);
    if (!seeded) {
        co_return std::unexpected(seeded.error());
    }

    std::vector<std::vector<std::byte>> buffers(kDepth, std::vector<std::byte>(kBlockSize));
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t round = 0; round < kTotalOps / kDepth; ++round) {
        const std::size_t base = round * kDepth;
        std::optional<std::error_code> first_error;
        kio::TaskGroup<> group(kDepth);
        for (std::size_t i = 0; i < kDepth; ++i) {
            group.Spawn(OneRead(ctx, fd, buffers[i], (base + i) * kBlockSize, first_error));
        }
        co_await group.JoinAll(ctx);
        if (first_error) {
            co_return std::unexpected(*first_error);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    co_return MakeBenchResult("kio streaming read depth64", kTotalOps, kTotalSize, elapsed);
}

kio::Task<kio::Result<BenchResult>> BenchAppendFdatasync(kio::IoContext& ctx, const std::filesystem::path path) {
    const auto fd_res = co_await kio::AsyncOpen(ctx, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (!fd_res) {
        co_return std::unexpected(fd_res.error());
    }
    const int fd = fd_res->Get();

    std::vector<std::byte> buf(kBlockSize, std::byte{0x4d});
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kSyncTotalOps; ++i) {
        auto res = co_await kio::AsyncWriteExact(ctx, fd, buf, i * kBlockSize);
        if (!res) {
            co_return std::unexpected(res.error());
        }
        if ((i + 1) % kSyncEvery == 0 || i + 1 == kSyncTotalOps) {
            auto sync = co_await kio::AsyncFdatasync(ctx, fd);
            if (!sync) {
                co_return std::unexpected(sync.error());
            }
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    co_return MakeBenchResult("kio append fdatasync/64", kSyncTotalOps, kSyncTotalSize, elapsed);
}

template <typename Factory>
BenchResult RunBench(const Factory& factory) {
    kio::IoContext ctx(kRingSize);
    std::optional<BenchResult> result;
    std::optional<std::error_code> error;
    ctx.RunUntilDone([&]() -> kio::Task<> {
        auto bench = co_await factory(ctx);
        if (!bench) {
            error = bench.error();
            co_return;
        }
        result = *bench;
    }());
    if (error) {
        throw std::system_error(*error);
    }
    return *result;
}

} // namespace

int main() {
    kio::alog::g_level = kio::alog::Level::Warn;

    const auto write_path = MakeTempPath("kio-bench-write");
    const auto read_path = MakeTempPath("kio-bench-read");

    PrintResult(RunBench([&](kio::IoContext& ctx) { return BenchSeqWrite(ctx, write_path); }));
    PrintResult(RunBench([&](kio::IoContext& ctx) { return BenchSeqRead(ctx, read_path); }));
    PrintResult(RunBench([&](kio::IoContext& ctx) { return BenchStreamingWrite(ctx, write_path); }));
    PrintResult(RunBench([&](kio::IoContext& ctx) { return BenchAppendFdatasync(ctx, write_path); }));
    PrintResult(RunBench([&](kio::IoContext& ctx) { return BenchStreamingRead(ctx, read_path); }));

    std::error_code ec;
    std::filesystem::remove(write_path, ec);
    std::filesystem::remove(read_path, ec);
    return 0;
}
