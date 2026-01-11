//
// Created by Yao ACHI on 03/01/2026.
//

#ifndef KIO_CHUNKED_BUFFER_H
#define KIO_CHUNKED_BUFFER_H
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <sys/uio.h>

namespace kio::io
{

class ChunkedBuffer
{
    static constexpr size_t kDefaultChunkSize = 8192;  // 8kB

    struct Chunk
    {
        std::unique_ptr<char[]> data;
        size_t capacity;
        size_t size{0};

        explicit Chunk(const size_t capacity) : data(std::make_unique<char[]>(capacity)), capacity(capacity) {}

        [[nodiscard]] size_t Available() const { return capacity - size; }
        [[nodiscard]] char* WritePtr() const { return data.get() + size; }
        [[nodiscard]] const char* Data() const { return data.get(); }
    };

public:
    explicit ChunkedBuffer(size_t chunk_size = kDefaultChunkSize) : chunk_size_(chunk_size) {}

    // --------------------
    // data producer side
    // ---------------------

    /**
     * @brief Append data to the buffer. The data remains uncommited
     * until Commit() is called.
     *
     * @param data Non-owned data to append.
     */
    void Append(std::span<const char> data);
    /**
     * @brief Append a string view (avoids null terminator issues with literals).
     */
    void Append(std::string_view sv) { Append(std::span(sv.data(), sv.size())); }

    // handle string literals
    void Append(const char* str)
    {
        // This will NOT include the null terminator
        Append(std::string_view(str));
    }
    /**
     * @brief Makes uncommited data visible to consumers (GetIoVects)
     */
    void Commit();
    /**
     * @brief Discards uncommited data.
     */
    void Rollback();
    // --------------------
    // data consumer side
    // ---------------------

    /**
     * @brief Return a vector of iovec representing all the commited data.
     * @return
     */
    [[nodiscard]] std::vector<iovec> GetIoVecs() const;
    [[nodiscard]] size_t CommittedSize() const { return committed_bytes_; }
    [[nodiscard]] bool HasData() const { return committed_bytes_ > 0; }
    void Consume(size_t bytes_consumed);

private:
    void EnsureSpace()
    {
        if (chunks_.empty() || chunks_.back()->Available() == 0)
        {
            chunks_.push_back(std::make_unique<Chunk>(chunk_size_));
        }
    }
    size_t chunk_size_;
    std::vector<std::unique_ptr<Chunk>> chunks_;
    size_t committed_bytes_{0};
    size_t uncommitted_bytes_{0};
};
}  // namespace kio::io

#endif  // KIO_CHUNKED_BUFFER_H
