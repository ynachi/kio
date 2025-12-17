//
// Created by Yao ACHI on 16/12/2025.
//

#ifndef KIO_ISTREAM_H
#define KIO_ISTREAM_H
#include "coro.h"
#include "errors.h"

namespace kio::io
{
    /**
     * @note Interface representing an IO stream
     */
    class IStream
    {
    public:
        virtual ~IStream() = default;
        virtual Task<Result<int>> async_read(std::span<char> buf) = 0;
        virtual Task<Result<void>> async_read_exact(std::span<char> buf) = 0;
        virtual Task<Result<int>> async_write(std::span<const char> buf) = 0;
        virtual Task<Result<void>> async_write_exact(std::span<const char> buf) = 0;
        virtual Task<Result<void>> async_shutdown() = 0;

    };

}

#endif  // KIO_ISTREAM_H
