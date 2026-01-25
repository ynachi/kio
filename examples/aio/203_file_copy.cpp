// examples/03_file_copy.cpp
// Demonstrates: AsyncOpen, AsyncRead, AsyncWrite, AsyncClose, Result handling

#include <array>
#include <print>

#include <fcntl.h>

#include "aio/aio.hpp"
#include <gflags/gflags.h>

DEFINE_string(src, "/toto/file.txt", "Source file");
DEFINE_string(dst, "/toto/file.txt", "Destination file");

namespace
{
aio::Task<aio::Result<size_t>> CopyFile(aio::IoContext& ctx, const char* src, const char* dst)
{
    auto src_fd = co_await aio::AsyncOpen(ctx, src, O_RDONLY, 0);
    if (!src_fd)
    {
        std::println(stderr, "Failed to open source: {}", src);
        co_return std::unexpected(src_fd.error());
    }

    auto dst_fd = co_await aio::AsyncOpen(ctx, dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!dst_fd)
    {
        co_await aio::AsyncClose(ctx, *src_fd);
        std::println(stderr, "Failed to open destination: {}", dst);
        co_return std::unexpected(dst_fd.error());
    }

    std::array<std::byte, 64 * 1024> buffer{};  // 64KB buffer
    size_t total_bytes = 0;
    uint64_t offset = 0;

    while (true)
    {
        auto read_result = co_await aio::AsyncRead(ctx, *src_fd, buffer, offset);
        if (!read_result)
        {
            co_await aio::AsyncClose(ctx, *src_fd);
            co_await aio::AsyncClose(ctx, *dst_fd);
            co_return std::unexpected(read_result.error());
        }

        if (*read_result == 0)
            break;  // EOF

        auto write_result = co_await aio::AsyncWrite(ctx, *dst_fd, std::span{buffer.data(), *read_result}, offset);
        if (!write_result)
        {
            co_await aio::AsyncClose(ctx, *src_fd);
            co_await aio::AsyncClose(ctx, *dst_fd);
            co_return std::unexpected(write_result.error());
        }

        offset += *read_result;
        total_bytes += *read_result;
    }

    co_await aio::AsyncClose(ctx, *src_fd);
    co_await aio::AsyncClose(ctx, *dst_fd);

    co_return total_bytes;
}

aio::Task<> AsyncMain(aio::IoContext& ctx, const char* src, const char* dst)
{
    std::println("Copying {} -> {}", src, dst);

    auto result = co_await CopyFile(ctx, src, dst);
    if (result)
    {
        std::println("Copied {} bytes", *result);
    }
    else
    {
        std::println(stderr, "Copy failed: {}", result.error().message());
    }
}
}  // namespace

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    aio::IoContext ctx;

    auto task = AsyncMain(ctx, FLAGS_src.c_str(), FLAGS_dst.c_str());
    ctx.RunUntilDone(task);

    return 0;
}