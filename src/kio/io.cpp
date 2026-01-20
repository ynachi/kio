#include "kio/io.hpp"

namespace kio::io
{

Task<Result<void>> sendfile(ThreadContext& ctx, int out_fd, int in_fd, off_t offset, size_t count)
{
    auto pipe_lease = detail::SplicePipePool::acquire();
    if (!pipe_lease)
        co_return std::unexpected(pipe_lease.error());

    const int pipe_rd = pipe_lease->read_fd;
    const int pipe_wr = pipe_lease->write_fd;

    size_t remaining = count;
    off_t current_offset = offset;

    while (remaining > 0)
    {
        constexpr size_t chunk_size = 65536;
        const size_t to_splice = std::min(remaining, chunk_size);

        auto res_in = co_await splice(ctx, in_fd, current_offset, pipe_wr, static_cast<off_t>(-1),
                                      static_cast<unsigned int>(to_splice), 0);
        if (!res_in)
            co_return std::unexpected(res_in.error());

        const int bytes_in = *res_in;
        if (bytes_in == 0)
            co_return error_from_errno(EPIPE);

        auto pipe_remaining = static_cast<size_t>(bytes_in);
        while (pipe_remaining > 0)
        {
            auto res_out = co_await splice(ctx, pipe_rd, static_cast<off_t>(-1), out_fd, static_cast<off_t>(-1),
                                           static_cast<unsigned int>(pipe_remaining), 0);
            if (!res_out)
                co_return std::unexpected(res_out.error());

            const int bytes_out = *res_out;
            if (bytes_out == 0)
                co_return error_from_errno(EPIPE);

            pipe_remaining -= static_cast<size_t>(bytes_out);
        }

        current_offset += bytes_in;
        remaining -= bytes_in;
    }
    co_return {};
}

}  // namespace kio::io