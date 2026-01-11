//
// Created by Yao ACHI on 03/01/2026.
//
#include "chunked_buffer.h"

#include <cassert>
#include <cstring>

namespace kio::io
{
void ChunkedBuffer::Append(std::span<const char> data)
{
    size_t remaining = data.size();
    const char* src = data.data();

    while (remaining > 0)
    {
        EnsureSpace();
        Chunk& current = *chunks_.back();

        const auto kToWrite = std::min(remaining, current.Available());
        std::memcpy(current.WritePtr(), src, kToWrite);

        current.size += kToWrite;
        src += kToWrite;
        remaining -= kToWrite;
        uncommitted_bytes_ += kToWrite;
    }
}

void ChunkedBuffer::Commit()
{
    committed_bytes_ += uncommitted_bytes_;
    uncommitted_bytes_ = 0;
}

void ChunkedBuffer::Rollback()
{
    if (chunks_.empty())
    {
        return;
    }

    size_t to_remove = uncommitted_bytes_;
    while (to_remove > 0 && !chunks_.empty())
    {
        if (Chunk& last = *chunks_.back(); last.size > to_remove)
        {
            last.size -= to_remove;
            to_remove = 0;
        }
        else
        {
            to_remove -= last.size;
            chunks_.pop_back();
        }
    }
    uncommitted_bytes_ = 0;
}

std::vector<iovec> ChunkedBuffer::GetIoVecs() const
{
    std::vector<iovec> iovs;
    iovs.reserve(chunks_.size());

    size_t bytes_left = committed_bytes_;
    for (const auto& chunk : chunks_)
    {
        if (bytes_left == 0)
        {
            break;
        }

        const auto kChunkBytes = std::min(bytes_left, chunk->size);
        iovs.push_back({.iov_base = chunk->data.get(), .iov_len = kChunkBytes});
        bytes_left -= kChunkBytes;
    }
    return iovs;
}

void ChunkedBuffer::Consume(size_t bytes_consumed)
{
    assert(bytes_consumed <= committed_bytes_);
    committed_bytes_ -= bytes_consumed;

    while (!chunks_.empty() && bytes_consumed >= chunks_.front()->size)
    {
        bytes_consumed -= chunks_.front()->size;
        chunks_.erase(chunks_.begin());
    }

    if (!chunks_.empty() && bytes_consumed > 0)
    {
        Chunk& head = *chunks_.front();
        std::memmove(head.data.get(), head.data.get() + bytes_consumed, head.size - bytes_consumed);
        head.size -= bytes_consumed;
    }
}

}  // namespace kio::io
