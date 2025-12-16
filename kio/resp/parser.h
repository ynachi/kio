//
// Created by Yao ACHI on 14/12/2025.
//

#ifndef KIO_RESP_PARSER_H
#define KIO_RESP_PARSER_H
#include <cstdint>
#include <expected>
#include <kio/core/bytes_mut.h>
#include <string_view>
#include <unistd.h>

namespace kio::resp
{
    struct ParserConfig
    {
        size_t max_size = 512 * 1024 * 1024;  // 512 MB
        size_t max_aggregate_depth = 32;
        size_t initial_buffer = 64 * 1024;  // 64 KB
    };

    /**
     * @brief RESP Data Types mapped to their protocol byte indicators.
     * @see https://github.com/redis/redis-specifications/blob/master/protocol/RESP3.md
     */
    enum class FrameType : uint8_t
    {
        // Simple types (CRLF-terminated)
        SimpleString = '+',  // +OK\r\n
        SimpleError = '-',  // -ERR message\r\n
        Integer = ':',  // :1000\r\n
        Null = '_',  // _\r\n
        Boolean = '#',  // #t\r\n or #f\r\n
        Double = ',',  // ,3.14\r\n
        BigNumber = '(',  // (123456789...\r\n

        // Bulk types (length-prefixed)
        BulkString = '$',  // $5\r\nhello\r\n
        BulkError = '!',  // !21\r\nSYNTAX error\r\n
        VerbatimString = '=',  // =15\r\ntxt:Some text\r\n

        // Aggregates (count + children)
        Array = '*',  // *2\r\n...
        Map = '%',  // %2\r\n...
        Set = '~',  // ~3\r\n...
        Push = '>',  // >3\r\n...
        Attribute = '|',  // |1\r\n...
    };

    struct FrameHeader
    {
        FrameType type;
        const char* data;  // Points into BytesMut
        size_t size;  // Total frame size including header & children
        size_t element_count;  // For aggregates: number of semantic elements
    };

    enum class ParseError
    {
        NeedMoreData,
        MalformedFrame,
        Atoi,
        MaxDepthReached,
        SizeOverflow,
        EoIter,
        Unknown
    };

    class Parser;

    // Zero-allocation iterator for streaming aggregate children
    class FrameIterator
    {
    public:
        explicit FrameIterator(const FrameHeader& parent, Parser& parser);

        // Get the next child frame
        std::expected<FrameHeader, ParseError> next();

        [[nodiscard]] bool has_next() const { return remaining_ > 0; }
        [[nodiscard]] size_t remaining() const { return remaining_; }

    private:
        Parser& parser_;
        const char* current_;
        size_t remaining_;
        // Logic depth for validation context
        size_t depth_;
    };

    // TODO: for now, skip that check
    // Validate no embedded CRLF in payload for strict compliance?
    // RESP3 allows loose compliance usually, but strict check is:
    // std::string_view payload{data.data() + 1, *pos - 1};
    // if (payload.find_first_of("\r\n") != std::string_view::npos) {
    //    last_error_ = "ERR malformed simple frame"; return std::nullopt;
    // }
    class Parser
    {
        friend class FrameIterator;

        BytesMut buffer_;
        ParserConfig config_;
        std::string last_error_;

        // Internal: parse frame starting at a given position
        std::expected<FrameHeader, ParseError> parse_frame_internal(std::span<const char> data, size_t depth);
        // helpers
        std::expected<FrameHeader, ParseError> extract_bulk_header(std::span<const char> data, FrameType type) const;

    public:
        explicit Parser(const ParserConfig& config) : buffer_(config.initial_buffer), config_(config) {}

        // Get buffer for network reads
        // WARNING: Modifying buffer invalidates all FrameView pointers!
        BytesMut& buffer() { return buffer_; }
        [[nodiscard]] const BytesMut& buffer() const { return buffer_; }

        // Try to parse the next complete frame from buffer
        // Returns nullptr if incomplete (need more data)
        // On success, frame points into buffer - valid until buffer modified
        // Warning: check if parser has error before in case of nulopt as response
        [[nodiscard]] std::expected<FrameHeader, ParseError> next_frame() { return parse_frame_internal(buffer_.readable_span(), 0); }

        // Consume bytes after processing a frame
        void consume(const size_t n) { buffer_.advance(n); }
        void consume(const FrameHeader& frame) { consume(frame.size); }
    };

    // --- Helpers ---
    std::string_view get_simple_payload(const FrameHeader& frame);
    std::string_view get_bulk_payload(const FrameHeader& frame);

}  // namespace kio::resp


#endif  // KIO_RESP_PARSER_H
