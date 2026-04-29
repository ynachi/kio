#include <algorithm>
#include <chrono>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <gflags/gflags.h>
#include <kio/core/task_group.hpp>
#include <kio/kio.hpp>

DEFINE_uint64(buf_size, 4096, "Buffer size for IO");
DEFINE_uint64(total_size, 256 * 1024 * 1024, "Total bytes to transfer");
DEFINE_uint64(depth, 64, "Depth for streaming IO");

using namespace std::chrono;

struct BenchResult
{
    uint64_t ops;
    uint64_t bytes;
    uint64_t elapsed_ns;
};

std::string get_temp_path(const char* prefix)
{
    auto now = system_clock::now().time_since_epoch().count();
    return std::string("/tmp/") + prefix + "_" + std::to_string(now) + ".bin";
}

kio::Task<BenchResult> run_sequential_write(kio::IoContext& ctx, int fd)
{
    std::vector<std::byte> buf(FLAGS_buf_size, std::byte{0x5a});
    size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < total_ops; ++i)
    {
        co_await kio::AsyncWrite(ctx, fd, std::span{buf}, i * FLAGS_buf_size);
    }
    auto end = high_resolution_clock::now();
    co_return {total_ops, FLAGS_total_size, (uint64_t)duration_cast<nanoseconds>(end - start).count()};
}

kio::Task<BenchResult> run_sequential_read(kio::IoContext& ctx, int fd)
{
    std::vector<std::byte> buf(FLAGS_buf_size);
    size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < total_ops; ++i)
    {
        co_await kio::AsyncRead(ctx, fd, std::span{buf}, i * FLAGS_buf_size);
    }
    auto end = high_resolution_clock::now();
    co_return {total_ops, FLAGS_total_size, (uint64_t)duration_cast<nanoseconds>(end - start).count()};
}

kio::Task<kio::Result<size_t>> wrap_write(kio::IoContext& ctx, int fd, std::span<const std::byte> buf, uint64_t off)
{
    co_return co_await kio::AsyncWrite(ctx, fd, buf, off);
}

kio::Task<kio::Result<size_t>> wrap_read(kio::IoContext& ctx, int fd, std::span<std::byte> buf, uint64_t off)
{
    co_return co_await kio::AsyncRead(ctx, fd, buf, off);
}

kio::Task<BenchResult> run_streaming_write(kio::IoContext& ctx, int fd)
{
    size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    size_t rounds = total_ops / FLAGS_depth;
    std::vector<std::vector<std::byte>> bufs(FLAGS_depth, std::vector<std::byte>(FLAGS_buf_size, std::byte{0x5a}));
    auto start = high_resolution_clock::now();
    for (size_t r = 0; r < rounds; ++r)
    {
        kio::TaskGroup<kio::Result<size_t>> group;
        for (size_t i = 0; i < FLAGS_depth; ++i)
        {
            size_t op_idx = r * FLAGS_depth + i;
            group.Spawn(wrap_write(ctx, fd, std::span{bufs[i]}, op_idx * FLAGS_buf_size));
        }
        co_await group.JoinAll(ctx);
    }
    auto end = high_resolution_clock::now();
    co_return {total_ops, FLAGS_total_size, (uint64_t)duration_cast<nanoseconds>(end - start).count()};
}

kio::Task<BenchResult> run_streaming_read(kio::IoContext& ctx, int fd)
{
    size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    size_t rounds = total_ops / FLAGS_depth;
    std::vector<std::vector<std::byte>> bufs(FLAGS_depth, std::vector<std::byte>(FLAGS_buf_size));
    auto start = high_resolution_clock::now();
    for (size_t r = 0; r < rounds; ++r)
    {
        kio::TaskGroup<kio::Result<size_t>> group;
        for (size_t i = 0; i < FLAGS_depth; ++i)
        {
            size_t op_idx = r * FLAGS_depth + i;
            group.Spawn(wrap_read(ctx, fd, std::span{bufs[i]}, op_idx * FLAGS_buf_size));
        }
        co_await group.JoinAll(ctx);
    }
    auto end = high_resolution_clock::now();
    co_return {total_ops, FLAGS_total_size, (uint64_t)duration_cast<nanoseconds>(end - start).count()};
}

void print_res(const char* label, const std::vector<BenchResult>& results)
{
    std::vector<uint64_t> samples;
    for (auto& r : results)
        samples.push_back(r.elapsed_ns);
    std::sort(samples.begin(), samples.end());
    uint64_t median = samples[samples.size() / 2];
    double secs = (double)median / 1e9;
    double ops_per_sec = (double)results[0].ops / secs;
    double mib_per_sec = ((double)results[0].bytes / (1024.0 * 1024.0)) / secs;
    printf("%s: median %.3f ms, %.0f ops/s, %.1f MiB/s\n", label, secs * 1000.0, ops_per_sec, mib_per_sec);
}

kio::Task<void> run_all_benches(kio::IoContext& ctx)
{
    auto path = get_temp_path("kio_bench");
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        perror("open");
        co_return;
    }

    printf("[KIO - 1 worker]\n");
    std::vector<BenchResult> results;
    for (int i = 0; i < 5; ++i)
        results.push_back(co_await run_sequential_write(ctx, fd));
    print_res("sequential write 4KiB", results);

    results.clear();
    for (int i = 0; i < 5; ++i)
        results.push_back(co_await run_sequential_read(ctx, fd));
    print_res("sequential read 4KiB", results);

    results.clear();
    for (int i = 0; i < 5; ++i)
        results.push_back(co_await run_streaming_write(ctx, fd));
    print_res("streaming write depth64 4KiB", results);

    results.clear();
    for (int i = 0; i < 5; ++i)
        results.push_back(co_await run_streaming_read(ctx, fd));
    print_res("streaming read depth64 4KiB", results);

    auto path_direct = get_temp_path("kio_direct_bench");
    int fd_d = ::open(path_direct.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd_d >= 0)
    {
        results.clear();
        for (int i = 0; i < 5; ++i)
            results.push_back(co_await run_sequential_write(ctx, fd_d));
        print_res("direct seq write 4KiB", results);
        ::close(fd_d);
        unlink(path_direct.c_str());
    }

    ::close(fd);
    unlink(path.c_str());
    co_return;
}

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    kio::IoContext ctx(512);
    ctx.RunUntilDone(run_all_benches(ctx));
    return 0;
}
