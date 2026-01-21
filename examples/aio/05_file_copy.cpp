// file copy + fixed-files demo
// Demonstrates: async_read/async_write with offsets, register_files + async_*_fixed
// Keeps it intentionally simple (sequential); great for correctness + baseline throughput.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

#include "aio/aio.hpp"

static int open_ro(const char* path) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    return fd;
}

static int open_wo(const char* path) {
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    return fd;
}

static off_t file_size(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) return -1;
    return st.st_size;
}

static aio::task<int> copy_task(aio::io_context& ctx,
                               int in_fd, int out_fd,
                               bool fixed_files,
                               size_t block_size) {
    off_t size = file_size(in_fd);
    if (size < 0) co_return 1;

    if (::ftruncate(out_fd, size) != 0) {
        std::perror("ftruncate");
        co_return 1;
    }

    std::vector<std::byte> buf(block_size);

    // If using fixed files, indices are:
    //   0 -> in_fd
    //   1 -> out_fd
    if (fixed_files) {
        for (off_t off = 0; off < size; ) {
            const size_t want = static_cast<size_t>(std::min<off_t>(size - off, (off_t)buf.size()));

            auto rr = co_await aio::async_read_fixed(&ctx, /*file_index=*/0, std::span(buf.data(), want), off);
            if (!rr) {
                std::println(stderr, "read_fixed failed: {}", rr.error().message());
                co_return 2;
            }

            size_t got = *rr;
            if (got == 0) break;

            size_t written = 0;
            while (written < got) {
                auto wr = co_await aio::async_write_fixed(
                    &ctx, /*file_index=*/1,
                    std::span<const std::byte>(buf.data() + written, got - written),
                    off + written);
                if (!wr) {
                    std::fprintf(stderr, "write_fixed failed: %s\n", wr.error().message().c_str());
                    co_return 3;
                }
                written += *wr;
            }

            off += got;
        }
    } else {
        for (off_t off = 0; off < size; ) {
            const size_t want = static_cast<size_t>(std::min<off_t>(size - off, (off_t)buf.size()));

            auto rr = co_await aio::async_read(&ctx, in_fd, buf.data(), want, off);
            if (!rr) {
                std::println(stderr, "read failed: {}", rr.error().message());
                co_return 2;
            }

            size_t got = *rr;
            if (got == 0) break;

            size_t written = 0;
            while (written < got) {
                auto wr = co_await aio::async_write(&ctx, out_fd, buf.data() + written, got - written, off + written);
                if (!wr) {
                    std::println(stderr, "write failed: {}", wr.error().message());
                    co_return 3;
                }
                written += *wr;
            }

            off += got;
        }
    }

    co_return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "usage: {} <in> <out> [block_kb=1024] [fixed=0|1]", argv[0]);
        return 1;
    }

    const char* in_path = argv[1];
    const char* out_path = argv[2];
    const size_t block_kb = (argc >= 4) ? static_cast<size_t>(std::atoi(argv[3])) : 1024;
    const bool fixed = (argc >= 5) ? (std::atoi(argv[4]) != 0) : false;

    int in_fd = open_ro(in_path);
    if (in_fd < 0) { std::perror("open in"); return 1; }

    int out_fd = open_wo(out_path);
    if (out_fd < 0) { std::perror("open out"); ::close(in_fd); return 1; }

    aio::io_context ctx(1024);

    if (fixed) {
        int fds[2] = { in_fd, out_fd };
        ctx.register_files(std::span<const int>(fds, 2));
    }

    auto t = copy_task(ctx, in_fd, out_fd, fixed, block_kb * 1024);
    ctx.run_until_done(t);

    int rc = t.result();
    ::close(in_fd);
    ::close(out_fd);
    return rc;
}
