// file copy + fixed-files demo - Modernized
// Demonstrates: async_read/async_write with offsets, register_files + async_*_fixed
// Updated to use: io_context& references, consistent std::span usage

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

#include "aio/io.hpp"
#include "aio/io_context.hpp"

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

// Modernized: io_context& instead of io_context*
static aio::Task<int> copy_task(aio::IoContext& ctx,
                                int in_fd, int out_fd,
                                bool fixed_files,
                                size_t block_size) {
    off_t size = file_size(in_fd);
    if (size < 0) co_return 1;

    if (::ftruncate(out_fd, size) != 0) {
        std::perror("ftruncate");
        co_return 1;
    }

    std::vector<std::byte> buffer(block_size);

    // If using fixed files, indices are:
    //   0 -> in_fd
    //   1 -> out_fd
    if (fixed_files) {
        for (off_t offset = 0; offset < size; ) {
            const size_t want = static_cast<size_t>(
                std::min<off_t>(size - offset, static_cast<off_t>(buffer.size()))
            );

            // Read chunk - using span subrange
            auto read_result = co_await aio::AsyncReadFixed(
                ctx,                                    // Reference, not pointer!
                /*file_index=*/0,
                std::span{buffer}.subspan(0, want),    // Explicit span subrange
                offset
            );

            if (!read_result) {
                std::println(stderr, "read_fixed failed: {}", read_result.error().message());
                co_return 2;
            }

            size_t got = *read_result;
            if (got == 0) break;  // EOF

            // Write chunk - may need multiple writes for partial results
            size_t written = 0;
            while (written < got) {
                auto write_result = co_await aio::AsyncWriteFixed(
                    ctx,                                                // Reference!
                    /*file_index=*/1,
                    std::span{buffer}.subspan(written, got - written), // Remaining bytes
                    offset + static_cast<off_t>(written)
                );

                if (!write_result) {
                    std::println(stderr, "write_fixed failed: {}",
                               write_result.error().message());
                    co_return 3;
                }

                written += *write_result;
            }

            offset += static_cast<off_t>(got);
        }
    } else {
        // Non-fixed file descriptors path
        for (off_t offset = 0; offset < size; ) {
            const size_t want = static_cast<size_t>(
                std::min<off_t>(size - offset, static_cast<off_t>(buffer.size()))
            );

            // Read chunk
            auto read_result = co_await aio::AsyncRead(
                ctx,                                 // Reference!
                in_fd,
                std::span{buffer}.subspan(0, want), // Span with exact size
                offset
            );

            if (!read_result) {
                std::println(stderr, "read failed: {}", read_result.error().message());
                co_return 2;
            }

            size_t got = *read_result;
            if (got == 0) break;  // EOF

            // Write chunk
            size_t written = 0;
            while (written < got) {
                auto write_result = co_await aio::AsyncWrite(
                    ctx,                                             // Reference!
                    out_fd,
                    std::span{buffer}.subspan(written, got - written), // Remaining
                    offset + static_cast<off_t>(written)
                );

                if (!write_result) {
                    std::println(stderr, "write failed: {}",
                               write_result.error().message());
                    co_return 3;
                }

                written += *write_result;
            }

            offset += static_cast<off_t>(got);
        }
    }

    co_return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "usage: {} <in> <out> [block_kb=1024] [fixed=0|1]",
                    argv[0]);
        return 1;
    }

    const char* in_path = argv[1];
    const char* out_path = argv[2];
    const size_t block_kb = (argc >= 4) ? static_cast<size_t>(std::atoi(argv[3])) : 1024;
    const bool use_fixed = (argc >= 5) ? (std::atoi(argv[4]) != 0) : false;

    int in_fd = open_ro(in_path);
    if (in_fd < 0) {
        std::perror("open input");
        return 1;
    }

    int out_fd = open_wo(out_path);
    if (out_fd < 0) {
        std::perror("open output");
        ::close(in_fd);
        return 1;
    }

    aio::IoContext ctx(1024);

    // Register files if using fixed file descriptors
    if (use_fixed) {
        std::array<int, 2> fds = { in_fd, out_fd };
        ctx.RegisterFiles(fds);  // std::array converts to span automatically
    }

    // Run the copy task
    auto task = copy_task(ctx, in_fd, out_fd, use_fixed, block_kb * 1024);
    ctx.RunUntilDone(task);

    int result = task.Result();

    // Cleanup
    ::close(in_fd);
    ::close(out_fd);

    return result;
}