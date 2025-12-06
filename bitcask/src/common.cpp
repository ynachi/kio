//
// Created by Yao ACHI on 14/11/2025.
//

#include "kio/core/worker.h"

using namespace kio;
using namespace kio::io;

namespace bitcask
{
    Task<Result<std::vector<char>>> read_file_content(Worker& io_worker, const int fd)
    {
        struct stat st{};
        // this is a blocking system call, but it's ok. We call this method only during database init.
        // Otherwise, do not use it anywhere else as it would block the whole event loop
        if (::fstat(fd, &st) < 0)
        {
            co_return std::unexpected(Error::from_errno(errno));
        }

        if (st.st_size == 0)
        {
            co_return std::vector<char>{};
        }

        std::vector<char> buffer(st.st_size);
        const std::span buf_span(buffer);

        KIO_TRY(co_await io_worker.async_read_exact_at(fd, buf_span, 0));

        co_return buffer;
    }

}  // namespace bitcask
