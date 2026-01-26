//
// Created by Yao ACHI on 25/01/2026.
//
#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <sys/uio.h>

namespace aio
{

class WritevBuffer
{
    static constexpr size_t kDefaultChunkSize = 8192;  // 8 KiB

    struct Chunk
    {
        std::unique_ptr<char[]> data;
        size_t capacity_;
        size_t size = 0;

        explicit Chunk(size_t capacity) : data(std::make_unique<char[]>(capacity)), capacity_(capacity) {}

        [[nodiscard]] size_t WritableBytes() const { return capacity_ - size; }
        [[nodiscard]] char* WritePtr() const { return data.get() + size; }  // matches original behavior
        [[nodiscard]] const char* Data() const { return data.get(); }
    };

public:
    explicit WritevBuffer(size_t chunk_size = kDefaultChunkSize) : chunk_size_(chunk_size) {}

    // --------------------
    // Producer side
    // --------------------

    // Append data to the buffer. The data remains uncommitted until Commit() is called.
    void Append(std::span<const char> data);

    void Append(std::string_view sv) { Append(std::span(sv.data(), sv.size())); }

    // String literal / C-string helper (does NOT include null terminator).
    void Append(const char* str) { Append(std::string_view(str)); }

    // Makes uncommitted data visible to consumers (IoVecs).
    void Commit()
    {
        committed_bytes_ += uncommitted_bytes_;
        uncommitted_bytes_ = 0;
    }

    // Discards uncommitted data.
    void RollbackPending();

    // --------------------
    // Consumer side
    // --------------------

    // Returns a vector of iovec representing all committed data.
    [[nodiscard]] std::vector<iovec> IoVecs() const;

    [[nodiscard]] size_t CommittedBytes() const { return committed_bytes_; }
    [[nodiscard]] bool HasData() const { return committed_bytes_ > 0; }

    void Consume(size_t bytes_consumed);

private:
    void EnsureSpace()
    {
        if (chunks_.empty() || chunks_.back()->WritableBytes() == 0)
        {
            chunks_.push_back(std::make_unique<Chunk>(chunk_size_));
        }
    }

    size_t chunk_size_;
    std::vector<std::unique_ptr<Chunk>> chunks_;
    size_t committed_bytes_ = 0;
    size_t uncommitted_bytes_ = 0;
};

}  // namespace aio
