#include <gflags/gflags.h>
#include <kio/kio.hpp>
#include <kio/core/task_group.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

DEFINE_uint64(buf_size, 4096, "Buffer size for IO");
DEFINE_uint64(total_size, 256 * 1024 * 1024, "Total bytes to transfer");
DEFINE_uint64(depth, 64, "Depth for streaming IO");

using namespace std::chrono;

namespace {

constexpr size_t kSampleCount = 5;
constexpr size_t kSyncTotalBytes = 64 * 1024 * 1024;
constexpr size_t kSyncEveryOps = 64;

struct BenchSample {
    uint64_t ops;
    uint64_t bytes;
    uint64_t ns;
};

struct Series {
    const char* label;
    uint64_t ops = 0;
    uint64_t bytes = 0;
    std::array<uint64_t, kSampleCount> samples_ns{};

    [[nodiscard]] uint64_t MedianNs() const
    {
        auto sorted = samples_ns;
        std::sort(sorted.begin(), sorted.end());
        return sorted[kSampleCount / 2];
    }
};

class AlignedBuffer {
public:
    AlignedBuffer() = default;

    AlignedBuffer(size_t size, size_t alignment) : size_(size)
    {
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, size) != 0) {
            throw std::bad_alloc();
        }
        ptr_ = static_cast<std::byte*>(ptr);
    }

    AlignedBuffer(AlignedBuffer&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)), size_(std::exchange(other.size_, 0))
    {
    }

    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept
    {
        if (this != &other) {
            Reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    ~AlignedBuffer() { Reset(); }

    [[nodiscard]] std::span<std::byte> Bytes() { return {ptr_, size_}; }
    [[nodiscard]] std::span<const std::byte> ConstBytes() const { return {ptr_, size_}; }

    void Fill(uint8_t value) const { std::memset(ptr_, value, size_); }

private:
    void Reset()
    {
        if (ptr_ != nullptr) {
            std::free(ptr_);
            ptr_ = nullptr;
            size_ = 0;
        }
    }

    std::byte* ptr_ = nullptr;
    size_t size_ = 0;
};

std::string GetTempPath(const char* prefix)
{
    const auto now = system_clock::now().time_since_epoch().count();
    return std::string("/tmp/") + prefix + "_" + std::to_string(now) + ".bin";
}

double OpsPerSec(const BenchSample& sample)
{
    return static_cast<double>(sample.ops) / (static_cast<double>(sample.ns) / 1e9);
}

double MiBPerSec(const BenchSample& sample)
{
    return (static_cast<double>(sample.bytes) / (1024.0 * 1024.0)) / (static_cast<double>(sample.ns) / 1e9);
}

void PrintSeries(const Series& series)
{
    const BenchSample sample{
        .ops = series.ops,
        .bytes = series.bytes,
        .ns = series.MedianNs(),
    };
    std::printf(
        "%s: median %.3f ms, %.0f ops/s, %.1f MiB/s\n",
        series.label,
        static_cast<double>(sample.ns) / 1e6,
        OpsPerSec(sample),
        MiBPerSec(sample)
    );
}

template <typename F>
kio::Task<kio::Result<size_t>> WriteExactTask(kio::IoContext& ctx, const F& file, std::span<const std::byte> buffer, uint64_t offset)
{
    auto result = co_await kio::AsyncWriteExact(ctx, file, buffer, offset);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return buffer.size();
}

template <typename F>
kio::Task<kio::Result<size_t>> ReadExactTask(kio::IoContext& ctx, const F& file, std::span<std::byte> buffer, uint64_t offset)
{
    auto result = co_await kio::AsyncReadExact(ctx, file, buffer, offset);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return buffer.size();
}

template <typename T>
kio::Result<void> CheckGroup(kio::TaskGroup<T>& group)
{
    for (auto& task : group.Tasks()) {
        auto result = task.Result();
        if constexpr (requires { result.has_value(); }) {
            if (!result) {
                return std::unexpected(result.error());
            }
        }
    }
    return kio::Result<void>{};
}

kio::Task<kio::Result<BenchSample>> BenchSequentialWrite(kio::IoContext& ctx, bool direct)
{
    const auto path = GetTempPath(direct ? "kio_direct_seq_write" : "kio_seq_write");
    const int flags = O_RDWR | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0);
    auto open_res = co_await kio::AsyncOpen(ctx, path, flags, 0644);
    if (!open_res) {
        co_return std::unexpected(open_res.error());
    }
    auto file = std::move(*open_res);

    const size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    const auto started = high_resolution_clock::now();
    if (direct) {
        AlignedBuffer buf(FLAGS_buf_size, FLAGS_buf_size);
        buf.Fill(0x5a);
        for (size_t i = 0; i < total_ops; ++i) {
            auto write_res = co_await kio::AsyncWriteExact(ctx, file, buf.ConstBytes(), i * FLAGS_buf_size);
            if (!write_res) {
                co_return std::unexpected(write_res.error());
            }
        }
    } else {
        std::vector<std::byte> buf(FLAGS_buf_size, std::byte{0x5a});
        for (size_t i = 0; i < total_ops; ++i) {
            auto write_res = co_await kio::AsyncWriteExact(ctx, file, std::span{buf}, i * FLAGS_buf_size);
            if (!write_res) {
                co_return std::unexpected(write_res.error());
            }
        }
    }
    const auto elapsed = duration_cast<nanoseconds>(high_resolution_clock::now() - started).count();

    auto close_res = co_await kio::AsyncClose(ctx, file);
    if (!close_res) {
        co_return std::unexpected(close_res.error());
    }
    if (::unlink(path.c_str()) != 0) {
        co_return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    co_return BenchSample{
        .ops = total_ops,
        .bytes = FLAGS_total_size,
        .ns = static_cast<uint64_t>(elapsed),
    };
}

kio::Task<kio::Result<BenchSample>> BenchSequentialRead(kio::IoContext& ctx, bool direct)
{
    const auto path = GetTempPath(direct ? "kio_direct_seq_read" : "kio_seq_read");
    const int flags = O_RDWR | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0);
    auto open_res = co_await kio::AsyncOpen(ctx, path, flags, 0644);
    if (!open_res) {
        co_return std::unexpected(open_res.error());
    }
    auto file = std::move(*open_res);

    const size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    if (direct) {
        AlignedBuffer seed(FLAGS_buf_size, FLAGS_buf_size);
        seed.Fill(0x31);
        for (size_t i = 0; i < total_ops; ++i) {
            auto write_res = co_await kio::AsyncWriteExact(ctx, file, seed.ConstBytes(), i * FLAGS_buf_size);
            if (!write_res) {
                co_return std::unexpected(write_res.error());
            }
        }

        AlignedBuffer read_buf(FLAGS_buf_size, FLAGS_buf_size);
        const auto started = high_resolution_clock::now();
        for (size_t i = 0; i < total_ops; ++i) {
            auto read_res = co_await kio::AsyncReadExact(ctx, file, read_buf.Bytes(), i * FLAGS_buf_size);
            if (!read_res) {
                co_return std::unexpected(read_res.error());
            }
        }
        const auto elapsed = duration_cast<nanoseconds>(high_resolution_clock::now() - started).count();

        auto close_res = co_await kio::AsyncClose(ctx, file);
        if (!close_res) {
            co_return std::unexpected(close_res.error());
        }
        if (::unlink(path.c_str()) != 0) {
            co_return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        co_return BenchSample{.ops = total_ops, .bytes = FLAGS_total_size, .ns = static_cast<uint64_t>(elapsed)};
    }

    std::vector<std::byte> seed(FLAGS_buf_size, std::byte{0x31});
    for (size_t i = 0; i < total_ops; ++i) {
        auto write_res = co_await kio::AsyncWriteExact(ctx, file, std::span{seed}, i * FLAGS_buf_size);
        if (!write_res) {
            co_return std::unexpected(write_res.error());
        }
    }

    std::vector<std::byte> read_buf(FLAGS_buf_size);
    const auto started = high_resolution_clock::now();
    for (size_t i = 0; i < total_ops; ++i) {
        auto read_res = co_await kio::AsyncReadExact(ctx, file, std::span{read_buf}, i * FLAGS_buf_size);
        if (!read_res) {
            co_return std::unexpected(read_res.error());
        }
    }
    const auto elapsed = duration_cast<nanoseconds>(high_resolution_clock::now() - started).count();

    auto close_res = co_await kio::AsyncClose(ctx, file);
    if (!close_res) {
        co_return std::unexpected(close_res.error());
    }
    if (::unlink(path.c_str()) != 0) {
        co_return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    co_return BenchSample{
        .ops = total_ops,
        .bytes = FLAGS_total_size,
        .ns = static_cast<uint64_t>(elapsed),
    };
}

kio::Task<kio::Result<BenchSample>> BenchStreamingWrite(kio::IoContext& ctx, bool direct)
{
    const auto path = GetTempPath(direct ? "kio_direct_stream_write" : "kio_stream_write");
    const int flags = O_RDWR | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0);
    auto open_res = co_await kio::AsyncOpen(ctx, path, flags, 0644);
    if (!open_res) {
        co_return std::unexpected(open_res.error());
    }
    auto file = std::move(*open_res);

    const size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    const size_t rounds = total_ops / FLAGS_depth;
    const auto started = high_resolution_clock::now();

    if (direct) {
        std::vector<AlignedBuffer> buffers;
        buffers.reserve(FLAGS_depth);
        for (size_t i = 0; i < FLAGS_depth; ++i) {
            buffers.emplace_back(FLAGS_buf_size, FLAGS_buf_size);
        }

        for (size_t round = 0; round < rounds; ++round) {
            kio::TaskGroup<kio::Result<size_t>> group(FLAGS_depth);
            for (size_t i = 0; i < FLAGS_depth; ++i) {
                const size_t op_index = round * FLAGS_depth + i;
                buffers[i].Fill(static_cast<uint8_t>(op_index & 0xff));
                group.Spawn(WriteExactTask(ctx, file, buffers[i].ConstBytes(), op_index * FLAGS_buf_size));
            }
            co_await group.JoinAll(ctx);
            auto group_res = CheckGroup(group);
            if (!group_res) {
                co_return std::unexpected(group_res.error());
            }
        }
    } else {
        std::vector<std::vector<std::byte>> buffers(FLAGS_depth, std::vector<std::byte>(FLAGS_buf_size));
        for (size_t round = 0; round < rounds; ++round) {
            kio::TaskGroup<kio::Result<size_t>> group(FLAGS_depth);
            for (size_t i = 0; i < FLAGS_depth; ++i) {
                const size_t op_index = round * FLAGS_depth + i;
                std::fill(buffers[i].begin(), buffers[i].end(), static_cast<std::byte>(op_index & 0xff));
                group.Spawn(WriteExactTask(ctx, file, std::span{buffers[i]}, op_index * FLAGS_buf_size));
            }
            co_await group.JoinAll(ctx);
            auto group_res = CheckGroup(group);
            if (!group_res) {
                co_return std::unexpected(group_res.error());
            }
        }
    }

    const auto elapsed = duration_cast<nanoseconds>(high_resolution_clock::now() - started).count();
    auto close_res = co_await kio::AsyncClose(ctx, file);
    if (!close_res) {
        co_return std::unexpected(close_res.error());
    }
    if (::unlink(path.c_str()) != 0) {
        co_return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    co_return BenchSample{.ops = total_ops, .bytes = FLAGS_total_size, .ns = static_cast<uint64_t>(elapsed)};
}

kio::Task<kio::Result<BenchSample>> BenchStreamingRead(kio::IoContext& ctx, bool direct)
{
    const auto path = GetTempPath(direct ? "kio_direct_stream_read" : "kio_stream_read");
    const int flags = O_RDWR | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0);
    auto open_res = co_await kio::AsyncOpen(ctx, path, flags, 0644);
    if (!open_res) {
        co_return std::unexpected(open_res.error());
    }
    auto file = std::move(*open_res);

    const size_t total_ops = FLAGS_total_size / FLAGS_buf_size;
    const size_t rounds = total_ops / FLAGS_depth;

    if (direct) {
        AlignedBuffer seed(FLAGS_buf_size, FLAGS_buf_size);
        seed.Fill(0x41);
        for (size_t i = 0; i < total_ops; ++i) {
            auto write_res = co_await kio::AsyncWriteExact(ctx, file, seed.ConstBytes(), i * FLAGS_buf_size);
            if (!write_res) {
                co_return std::unexpected(write_res.error());
            }
        }

        std::vector<AlignedBuffer> buffers;
        buffers.reserve(FLAGS_depth);
        for (size_t i = 0; i < FLAGS_depth; ++i) {
            buffers.emplace_back(FLAGS_buf_size, FLAGS_buf_size);
        }

        const auto started = high_resolution_clock::now();
        for (size_t round = 0; round < rounds; ++round) {
            kio::TaskGroup<kio::Result<size_t>> group(FLAGS_depth);
            for (size_t i = 0; i < FLAGS_depth; ++i) {
                const size_t op_index = round * FLAGS_depth + i;
                group.Spawn(ReadExactTask(ctx, file, buffers[i].Bytes(), op_index * FLAGS_buf_size));
            }
            co_await group.JoinAll(ctx);
            auto group_res = CheckGroup(group);
            if (!group_res) {
                co_return std::unexpected(group_res.error());
            }
        }
        const auto elapsed = duration_cast<nanoseconds>(high_resolution_clock::now() - started).count();
        auto close_res = co_await kio::AsyncClose(ctx, file);
        if (!close_res) {
            co_return std::unexpected(close_res.error());
        }
        if (::unlink(path.c_str()) != 0) {
            co_return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        co_return BenchSample{.ops = total_ops, .bytes = FLAGS_total_size, .ns = static_cast<uint64_t>(elapsed)};
    }

    std::vector<std::byte> seed(FLAGS_buf_size, std::byte{0x41});
    for (size_t i = 0; i < total_ops; ++i) {
        auto write_res = co_await kio::AsyncWriteExact(ctx, file, std::span{seed}, i * FLAGS_buf_size);
        if (!write_res) {
            co_return std::unexpected(write_res.error());
        }
    }

    std::vector<std::vector<std::byte>> buffers(FLAGS_depth, std::vector<std::byte>(FLAGS_buf_size));
    const auto started = high_resolution_clock::now();
    for (size_t round = 0; round < rounds; ++round) {
        kio::TaskGroup<kio::Result<size_t>> group(FLAGS_depth);
        for (size_t i = 0; i < FLAGS_depth; ++i) {
            const size_t op_index = round * FLAGS_depth + i;
            group.Spawn(ReadExactTask(ctx, file, std::span{buffers[i]}, op_index * FLAGS_buf_size));
        }
        co_await group.JoinAll(ctx);
        auto group_res = CheckGroup(group);
        if (!group_res) {
            co_return std::unexpected(group_res.error());
        }
    }
    const auto elapsed = duration_cast<nanoseconds>(high_resolution_clock::now() - started).count();

    auto close_res = co_await kio::AsyncClose(ctx, file);
    if (!close_res) {
        co_return std::unexpected(close_res.error());
    }
    if (::unlink(path.c_str()) != 0) {
        co_return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    co_return BenchSample{.ops = total_ops, .bytes = FLAGS_total_size, .ns = static_cast<uint64_t>(elapsed)};
}

kio::Task<kio::Result<BenchSample>> BenchAppendFdatasync(kio::IoContext& ctx)
{
    const auto path = GetTempPath("kio_append_sync");
    auto open_res = co_await kio::AsyncOpen(ctx, path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (!open_res) {
        co_return std::unexpected(open_res.error());
    }
    auto file = std::move(*open_res);

    const size_t total_ops = kSyncTotalBytes / FLAGS_buf_size;
    std::vector<std::byte> buf(FLAGS_buf_size, std::byte{0x4d});

    const auto started = high_resolution_clock::now();
    for (size_t i = 0; i < total_ops; ++i) {
        auto write_res = co_await kio::AsyncWriteExact(ctx, file, std::span{buf}, i * FLAGS_buf_size);
        if (!write_res) {
            co_return std::unexpected(write_res.error());
        }
        if ((i + 1) % kSyncEveryOps == 0 || i + 1 == total_ops) {
            auto sync_res = co_await kio::AsyncFdatasync(ctx, file);
            if (!sync_res) {
                co_return std::unexpected(sync_res.error());
            }
        }
    }
    const auto elapsed = duration_cast<nanoseconds>(high_resolution_clock::now() - started).count();

    auto close_res = co_await kio::AsyncClose(ctx, file);
    if (!close_res) {
        co_return std::unexpected(close_res.error());
    }
    if (::unlink(path.c_str()) != 0) {
        co_return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    co_return BenchSample{.ops = total_ops, .bytes = kSyncTotalBytes, .ns = static_cast<uint64_t>(elapsed)};
}

template <typename Factory>
kio::Task<kio::Result<Series>> SampleCase(kio::IoContext& ctx, const char* label, Factory&& factory)
{
    Series series{.label = label};
    for (size_t i = 0; i < kSampleCount; ++i) {
        auto result = co_await factory(ctx);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        series.ops = result->ops;
        series.bytes = result->bytes;
        series.samples_ns[i] = result->ns;
    }
    co_return series;
}

kio::Task<kio::Result<void>> RunBench(kio::IoContext& ctx)
{
    std::printf("kio storage benchmarks (normalized)\n");
    std::printf("target=linux-ish sample_count=%zu\n\n", kSampleCount);
    std::printf("[kio_io_uring]\n");

    const auto seq_write = co_await SampleCase(ctx, "sequential write 4KiB", [](kio::IoContext& inner) {
        return BenchSequentialWrite(inner, false);
    });
    if (!seq_write) co_return std::unexpected(seq_write.error());
    PrintSeries(*seq_write);

    const auto seq_read = co_await SampleCase(ctx, "sequential read 4KiB", [](kio::IoContext& inner) {
        return BenchSequentialRead(inner, false);
    });
    if (!seq_read) co_return std::unexpected(seq_read.error());
    PrintSeries(*seq_read);

    const auto stream_write = co_await SampleCase(ctx, "streaming write depth64 4KiB", [](kio::IoContext& inner) {
        return BenchStreamingWrite(inner, false);
    });
    if (!stream_write) co_return std::unexpected(stream_write.error());
    PrintSeries(*stream_write);

    const auto stream_read = co_await SampleCase(ctx, "streaming read depth64 4KiB", [](kio::IoContext& inner) {
        return BenchStreamingRead(inner, false);
    });
    if (!stream_read) co_return std::unexpected(stream_read.error());
    PrintSeries(*stream_read);

    const auto sync_case = co_await SampleCase(ctx, "append fdatasync/64 4KiB", [](kio::IoContext& inner) {
        return BenchAppendFdatasync(inner);
    });
    if (!sync_case) co_return std::unexpected(sync_case.error());
    PrintSeries(*sync_case);

    const auto direct_seq_write = co_await SampleCase(ctx, "direct seq write 4KiB", [](kio::IoContext& inner) {
        return BenchSequentialWrite(inner, true);
    });
    if (!direct_seq_write) co_return std::unexpected(direct_seq_write.error());
    PrintSeries(*direct_seq_write);

    const auto direct_seq_read = co_await SampleCase(ctx, "direct seq read 4KiB", [](kio::IoContext& inner) {
        return BenchSequentialRead(inner, true);
    });
    if (!direct_seq_read) co_return std::unexpected(direct_seq_read.error());
    PrintSeries(*direct_seq_read);

    const auto direct_stream_write = co_await SampleCase(ctx, "direct streaming write depth64 4KiB", [](kio::IoContext& inner) {
        return BenchStreamingWrite(inner, true);
    });
    if (!direct_stream_write) co_return std::unexpected(direct_stream_write.error());
    PrintSeries(*direct_stream_write);

    const auto direct_stream_read = co_await SampleCase(ctx, "direct streaming read depth64 4KiB", [](kio::IoContext& inner) {
        return BenchStreamingRead(inner, true);
    });
    if (!direct_stream_read) co_return std::unexpected(direct_stream_read.error());
    PrintSeries(*direct_stream_read);

    std::printf("\n");
    co_return kio::Result<void>{};
}

}  // namespace

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    kio::IoContext ctx;
    ctx.RunUntilDone([](kio::IoContext& inner) -> kio::Task<void> {
        auto result = co_await RunBench(inner);
        if (!result) {
            std::fprintf(stderr, "benchmark failed: %s\n", result.error().message().c_str());
            std::exit(1);
        }
    }(ctx));

    return 0;
}
