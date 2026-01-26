//
// Created by Yao ACHI on 25/01/2026.
//

#include "aio/io_buffer.hpp"

namespace aio
{
void IoBuffer::EnsureWritableBytes(size_t additional)
{
    if (WritableBytes() >= additional)
    {
        return;
    }

    const size_t live_bytes = ReadableBytes();
    const size_t total_capacity = data_.capacity();

    // Strategy 1: Compact in place if it fits.
    if (total_capacity >= live_bytes + additional)
    {
        // Only compact if there is significant waste.
        if (read_offset_ >= kAutoCompactionThresholdBytes || read_offset_ >= data_.size() / 4)
        {
            Compact();
        }

        if (data_.size() < write_offset_ + additional)
        {
            data_.resize(write_offset_ + additional);
        }
        return;
    }

    // Strategy 2: Reallocate; copy ONLY live data.
    const size_t new_capacity = std::max(total_capacity * 2, live_bytes + additional);
    std::vector<char> new_buffer;
    new_buffer.reserve(new_capacity);
    new_buffer.resize(new_capacity);

    if (live_bytes > 0)
    {
        std::memcpy(new_buffer.data(), data_.data() + read_offset_, live_bytes);
    }

    data_ = std::move(new_buffer);
    read_offset_ = 0;
    write_offset_ = live_bytes;
}
}  // namespace aio